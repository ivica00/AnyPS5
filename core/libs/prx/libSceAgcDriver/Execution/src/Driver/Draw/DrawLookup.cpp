#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/DeferredLabels.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/UnitShadow.hpp"
#include <algorithm>

namespace AgcDriver::DriverDetail {

namespace {

// True when the stamp taken after the last Equal compare still covers this variant: the publish,
// pending-image, forget and shadow serials are the ones sampled around that compare, no deferred
// label is queued, and a collect of every span shows no store since the stamped generation.
bool validationStampHolds(const DispatchVariant& variant) {
    const auto generation = variant.generation.load(std::memory_order_acquire);
    if (generation == 0) return false;
    if (variant.spans.empty() && !variant.runs.empty()) return false;
    if (Graphics::Recorder::PublishGeneration() != variant.proofPublish.load(std::memory_order_acquire)) return false;
    if (Graphics::StorageTexture::PendingSerial() != variant.proofPending.load(std::memory_order_acquire)) return false;
    if (GuestMemory::ForgetSerial() != variant.proofForget.load(std::memory_order_acquire)) return false;
    if ((variant.proofForget.load(std::memory_order_acquire) & 1ull) != 0) return false;
    if (!deferredLabels().labels.empty()) return false;
    if (Graphics::ShadowSerial() != variant.proofShadow.load(std::memory_order_acquire)) return false;
    if (variant.spans.empty()) return true;
    thread_local std::vector<GuestMemory::UnchangedQuery> queries;
    queries.clear();
    queries.reserve(variant.spans.size());
    for (const auto& [begin, bytes] : variant.spans) {
        if (GuestMemory::CollectWrites(begin, bytes) == 0) return false;
        queries.push_back({begin, bytes, generation});
    }
    return GuestMemory::UnchangedSinceAll(queries);
}

bool collectSpanGeneration(const DispatchVariant& variant, std::uint64_t& collected) {
    if (variant.spans.empty()) {
        if (!variant.runs.empty()) return false;
        collected = std::max<std::uint64_t>(GuestMemory::TrackerGeneration(), 1);
        return true;
    }
    collected = 0;
    for (const auto& [begin, bytes] : variant.spans) {
        const auto generation = GuestMemory::CollectWrites(begin, bytes);
        if (generation == 0) return false;
        if (generation > collected) collected = generation;
    }
    return collected != 0;
}

void storeValidationStamp(DispatchVariant& variant, std::uint64_t collected, std::uint64_t publish, std::uint64_t pending, std::uint64_t forget, std::uint64_t shadow) {
    if (collected == 0 || (forget & 1ull) != 0 || (variant.spans.empty() && !variant.runs.empty())) {
        variant.generation.store(0, std::memory_order_release);
        return;
    }
    variant.proofPublish.store(publish, std::memory_order_relaxed);
    variant.proofPending.store(pending, std::memory_order_relaxed);
    variant.proofForget.store(forget, std::memory_order_relaxed);
    variant.proofShadow.store(shadow, std::memory_order_relaxed);
    variant.generation.store(collected, std::memory_order_release);
}

}

void Driver::lookupDraw(const Submission& submission, const std::shared_ptr<VulkanDevice>& localDevice, const Graphics::State& graphics, const ShaderRecompiler::ShaderPixelStageInfo& pixel, const std::vector<DrawProgram>& programs, const std::vector<ShaderRecompiler::ProgramRole>& roles, const std::vector<std::optional<ShaderRecompiler::ShaderVertexStageInfo>>& vertexInfos, bool useDrawEntries, bool registerKey, bool profile, std::uint64_t& drawKey, std::shared_ptr<DrawEntry>& entry, std::vector<std::shared_ptr<DispatchVariant>>& matched, std::vector<std::vector<ShaderRecompiler::MemoryRegion>>& matchedRegions, bool& drawHit, bool& verifyHit, DrawPhaseTiming& phaseTiming, std::array<double, DrawDriverPhaseCount>& phaseMs, const std::shared_ptr<DrawEntry>& dataEntry, DrawDataCandidates& dataCandidates, std::vector<StageDataHit>& stageData) {
    using Role = ShaderRecompiler::ProgramRole;
    if (useDrawEntries) {
        std::uint64_t stampHits = 0, stampMisses = 0;
        // Collects the spans first, then compares. The generation stored on Equal is the one from
        // that collect, so a store landing after the compare is stamped newer and the next lookup
        // misses. A moved publish or pending serial is retried once, with its own collect.
        const auto checkVariant = [&](std::uint64_t program, DispatchVariant& variant, std::span<const ShaderRecompiler::MemoryRegion> regions, std::uint64_t& imagesFlushed, std::uint64_t& runsSynced, std::optional<SampledReadScope>& sampling, std::vector<std::pair<std::uint32_t, std::uint32_t>>* live = nullptr) {
            if (drawStampValidate() && validationStampHolds(variant)) {
                ++stampHits;
                return EntryOutcome::Equal;
            }
            if (drawStampValidate()) ++stampMisses;
            const auto attempt = [&] {
                std::uint64_t collected = 0;
                const bool collectOk = drawStampValidate() && collectSpanGeneration(variant, collected);
                const auto publish = Graphics::Recorder::PublishGeneration();
                const auto pending = Graphics::StorageTexture::PendingSerial();
                const auto forget = GuestMemory::ForgetSerial();
                const auto shadow = Graphics::ShadowSerial();
                const auto result = validateVariant(program, submission.queue, variant, regions, imagesFlushed, runsSynced, sampling, live);
                if (result == EntryOutcome::Equal && collectOk && deferredLabels().labels.empty() && Graphics::Recorder::PublishGeneration() == publish && Graphics::StorageTexture::PendingSerial() == pending && GuestMemory::ForgetSerial() == forget && Graphics::ShadowSerial() == shadow) {
                    storeValidationStamp(variant, collected, publish, pending, forget, shadow);
                } else if (result != EntryOutcome::Equal && result != EntryOutcome::PublishMoved && result != EntryOutcome::PendingMoved) {
                    variant.generation.store(0, std::memory_order_release);
                }
                return result;
            };
            auto result = attempt();
            if (gateRetry() && (result == EntryOutcome::PublishMoved || result == EntryOutcome::PendingMoved)) result = attempt();
            return result;
        };
        if (!registerKey) {
            drawKey = 0xcbf29ce484222325ull;
            const auto mix = [&](std::uint64_t value) {
                drawKey ^= value;
                drawKey *= 0x100000001b3ull;
            };
            mix(localDevice->Serial());
            mix(static_cast<std::uint64_t>(graphics.stages.path));
            mix(graphics.stages.registerValue);
            mix(graphics.stages.vertexWaveSize);
            mix(graphics.stages.fragmentWaveSize);
            mix(graphics.stages.mesh.has_value());
            if (graphics.stages.mesh) {
                const auto& mesh = *graphics.stages.mesh;
                for (const auto value : {mesh.inputPrimitive, mesh.primitivesPerGroup, mesh.verticesPerGroup, mesh.maxVertices, mesh.maxPrimitives, mesh.threadsPerGroup, mesh.ldsSizeDwords, mesh.provokingVertex, mesh.esgsItemSize}) mix(value);
            }
            mix(graphics.stages.tessellation.has_value());
            if (graphics.stages.tessellation) {
                const auto& tess = *graphics.stages.tessellation;
                for (const auto value : {tess.inputControlPoints, tess.outputControlPoints, tess.domain, tess.partitioning, tess.outputTopology, tess.patchesPerGroup, tess.primitiveGroupSize, static_cast<std::uint32_t>(tess.breakAtInstance)}) mix(value);
            }
            mix(pixel.layerExported);
            mix(graphics.rectList);
            mix(programs.size());
            for (std::size_t i = 0; i < programs.size(); ++i) {
                const auto& program = programs[i];
                mix(reinterpret_cast<std::uintptr_t>(program.snapshot.get()));
                mix(program.codeOffset);
                mix(static_cast<std::uint64_t>(roles[i]));
                mix(static_cast<std::uint64_t>(program.binary.stage));
                mix(program.userDataBase);
                mix(program.firstUserSgpr);
                mix(program.userData.size());
                for (const auto word : program.userData) mix(word);
                mix(vertexInfos[i].has_value());
                if (!vertexInfos[i]) continue;
                const auto& vertex = *vertexInfos[i];
                require(vertex.resourcesNum <= vertex.resources.size(), "vertex stage info resource count exceeds its table");
                mix(vertex.resourcesNum);
                mix(vertex.fetchAttribReg);
                mix(vertex.fetchBufferReg);
                mix(vertex.fetchEmbedded);
                mix(vertex.paClVsOutCntl);
                for (std::uint32_t r = 0; r < vertex.resourcesNum; ++r) {
                    for (const auto field : vertex.resources[r].fields) mix(field);
                    const auto& destination = vertex.resourcesDst[r];
                    mix(static_cast<std::uint32_t>(destination.registerStart));
                    mix(static_cast<std::uint32_t>(destination.registersNum));
                    mix(static_cast<std::uint32_t>(destination.attrId));
                    mix(destination.fetchIndex);
                }
            }
            require(pixel.interpolatorCount <= pixel.interpolatorSettings.size(), "pixel stage info interpolator count exceeds its table");
            mix(pixel.interpolatorCount);
            for (std::uint32_t i = 0; i < pixel.interpolatorCount; ++i) mix(pixel.interpolatorSettings[i]);
            mix(pixel.inputAddr);
            for (const bool flag : {pixel.wave32, pixel.hasPerspectiveCenterVgpr, pixel.perspectiveCentroid, pixel.posX, pixel.posY, pixel.posZ, pixel.posW, pixel.frontFace, pixel.frontFaceAllBits, pixel.ancillary, pixel.sampleShading, pixel.noPerspective, pixel.linearCentroid, pixel.pixelKillEnable, pixel.depthExportEnable, pixel.sampleMaskExportEnable, pixel.earlyZ, pixel.executeOnNoop}) mix(flag);
            mix(static_cast<std::uint64_t>(pixel.conservativeZExport));
            mix(pixel.orderedPixelShader);
            mix(pixel.stencilRefExportEnable);
            for (const auto value : pixel.targetOutputMode) mix(value);
            for (const auto value : pixel.targetExportMapping) mix(value);
            for (const auto value : pixel.targetExportPacking) mix(static_cast<std::uint64_t>(value));
            mix(pixel.dualSourceBlend);
            mix(pixel.alphaToCoverage);
            mix(static_cast<std::uint64_t>(pixel.rasterPrimitive));
            mix(pixel.ditheredAlphaToCoverage);
            mix(pixel.quadPixelMask);
            for (const auto mask : pixel.quadSampleMask) mix(mask);
            std::lock_guard cacheLock(drawCacheMutex);
            ++drawEntryCounters.lookups;
            const auto found = drawCache.find(drawKey);
            if (found != drawCache.end()) entry = found->second;
            else ++drawEntryCounters.absent;
            maybeReportDrawCache(profile);
        }
        if (entry != nullptr) {
            const auto waitedBeforeValidate = profile ? Graphics::Recorder::ThreadWaitedMs() : 0.0;
            std::optional<DrawMiss> miss;
            std::vector<std::size_t> ranks(programs.size(), 0);
            std::uint64_t stageValidations = 0, stageEqual = 0, compared = 0, imagesFlushed = 0, runsSynced = 0, dataStages = 0, dataWords = 0;
            std::vector<std::pair<std::uint32_t, std::uint32_t>> liveData;
            auto* live = drawStageDataHits() ? &liveData : nullptr;
            if (entry->stages.size() != programs.size()) miss = DrawMiss::Stages;

            std::uint32_t cursor = 0;
            {
                const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::DrawCache);

                std::optional<SampledReadScope> sampling;
                for (std::size_t i = 0; !miss && i < programs.size(); ++i) {
                    if (roles[i] == Role::GeometryBack) continue;
                    ++stageValidations;
                    cursor = Graphics::StagePushOffset(cursor, programs[i].binary.stage, localDevice->GraphicsPipelineLibraries());
                    const auto& variants = entry->stages[i];
                    auto outcome = EntryOutcome::Differing;
                    bool anyLayout = false;
                    for (std::size_t rank = 0; rank < variants.size(); ++rank) {
                        const auto& variant = variants[rank];
                        if (variant->pushOffset != cursor) continue;
                        auto& regions = matchedRegions[i];
                        regions.clear();
                        appendEntryRegions(*variant, regions);
                        ++compared;
                        auto result = checkVariant(programs[i].binary.codeAddress, *variant, regions, imagesFlushed, runsSynced, sampling, live);
                        if (!anyLayout) outcome = result;
                        anyLayout = true;
                        if (result != EntryOutcome::Equal && result != EntryOutcome::EqualData) continue;
                        if (result == EntryOutcome::EqualData) {
                            auto& hit = stageData[i];
                            hit.words = variant->words;
                            for (const auto& [position, value] : liveData) hit.words[position] = value;
                            regions.clear();
                            appendEntryRegions(*variant, regions, &hit.words);
                            hit.compiled = patchDataWords(*variant, hit.words);
                            ++dataStages;
                            dataWords += liveData.size();
                        }
                        matched[i] = variant;
                        ranks[i] = rank;
                        ++stageEqual;
                        cursor += static_cast<std::uint32_t>(variant->compiled->pushConstants.size());
                        break;
                    }
                    if (matched[i] != nullptr) continue;
                    if (!anyLayout) miss = DrawMiss::Layout;
                    else if (outcome != EntryOutcome::Differing) miss = DrawMiss::Gate;
                    else miss = roles[i] == Role::Fragment ? DrawMiss::FragmentDiffering : i == 0 ? DrawMiss::FrontDiffering : DrawMiss::OtherDiffering;
                }
            }
            drawHit = !miss;
            std::lock_guard cacheLock(drawCacheMutex);
            auto& counters = drawEntryCounters;
            counters.stageValidations += stageValidations;
            counters.stageEqual += stageEqual;
            counters.variantsCompared += compared;
            counters.stampHits += stampHits;
            counters.stampMisses += stampMisses;
            if (drawHit) {
                ++counters.hits;
                counters.stageDataHits += dataStages;
                counters.stageDataWords += dataWords;
                if (registerKey) ++counters.registerKeyHits;
                ++drawCacheHits;
                bool rotate = false;
                for (std::size_t i = 0; i < programs.size(); ++i) {
                    if (matched[i] == nullptr) continue;
                    ++counters.variantHitsByRank[ranks[i]];
                    if (ranks[i] != 0) rotate = true;
                }

                const auto again = drawCache.find(drawKey);
                if (again != drawCache.end() && again->second == entry) {
                    if (rotate) {
                        auto rotated = std::make_shared<DrawEntry>();
                        rotated->decode = entry->decode;
                        rotated->stages = entry->stages;
                        rotated->shape = entry->shape;
                        rotated->recipes.store(entry->recipes.load());
                        for (std::size_t i = 0; i < programs.size(); ++i) {
                            if (ranks[i] == 0) continue;
                            auto& variants = rotated->stages[i];
                            variants.erase(variants.begin() + static_cast<std::ptrdiff_t>(ranks[i]));
                            variants.insert(variants.begin(), matched[i]);
                        }
                        rotated->touched = entry->touched;
                        rotated->order = entry->order;
                        again->second = std::move(rotated);
                    }
                    if (drawCacheHits - again->second->touched > drawCacheEntries() / 8) {
                        drawOrder.splice(drawOrder.begin(), drawOrder, again->second->order);
                        again->second->touched = drawCacheHits;
                        ++counters.touches;
                    }
                }
                if (verifyDrawEntries()) {
                    ++counters.verifyHits;
                    verifyHit = true;
                    drawHit = false;
                }
                if (dataStages != 0 && verifyDrawDataHits()) drawHit = false;
            } else {
                ++counters.misses[static_cast<std::size_t>(*miss)];
                if (registerKey && entry->decode != nullptr) ++counters.decodePartial;
            }
            if (profile) {

                phaseTiming.Phase(DrawRowKeyLookupValidate);
                const auto waited = std::min(Graphics::Recorder::ThreadWaitedMs() - waitedBeforeValidate, phaseMs[DrawRowKeyLookupValidate]);
                phaseMs[DrawRowKeyLookupValidate] -= waited;
                phaseMs[DrawRowValidateWait] += waited;
                counters.validateUs += phaseMs[DrawRowKeyLookupValidate] * 1000;
            }
        } else if (dataEntry != nullptr && graphics.stages.path == Graphics::ShaderPath::Vertex && dataEntry->stages.size() == programs.size()) {
            std::uint64_t imagesFlushed = 0, runsSynced = 0;
            const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::DrawCache);
            std::optional<SampledReadScope> sampling;
            for (std::size_t i = 0; i < programs.size(); ++i) {
                if (roles[i] == Role::GeometryBack) continue;
                auto& candidates = dataCandidates[i];
                for (const auto& variant : dataEntry->stages[i]) {
                    if ((i == 0 && variant->pushOffset != 0) || std::any_of(candidates.begin(), candidates.end(), [&](const auto& candidate) { return candidate.first->pushOffset == variant->pushOffset; })) continue;
                    if (!canReuseWithUserData(*variant, programs[i])) continue;
                    std::vector<ShaderRecompiler::MemoryRegion> regions;
                    appendEntryRegions(*variant, regions);
                    auto result = checkVariant(programs[i].binary.codeAddress, *variant, regions, imagesFlushed, runsSynced, sampling);
                    if (result == EntryOutcome::Equal) {
                        candidates.emplace_back(variant, std::move(regions));
                        break;
                    }
                }
            }
            if (stampHits != 0 || stampMisses != 0) {
                std::lock_guard cacheLock(drawCacheMutex);
                drawEntryCounters.stampHits += stampHits;
                drawEntryCounters.stampMisses += stampMisses;
            }
        }
        phaseTiming.Phase(DrawRowKeyLookupValidate);
    }
}

bool Driver::canReuseWithUserData(const DispatchVariant& variant, const DrawProgram& program) {
    return variant.userData == program.userData;
}

}
