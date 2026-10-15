#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawCache.hpp"
#include <algorithm>
#include <bit>
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/DeferredLabels.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Memory/WriteEvidence.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <cstdlib>

namespace AgcDriver::DriverDetail {

bool DrawRecipeRecord::Matches(const std::vector<std::shared_ptr<DispatchVariant>>& variants) const {
    if (stages.size() != variants.size()) return false;
    for (std::size_t i = 0; i < stages.size(); ++i) {
        if (stages[i].owner_before(variants[i]) || variants[i].owner_before(stages[i])) return false;
    }
    return true;
}

bool DrawRecipeRecord::Expired() const {
    return std::any_of(stages.begin(), stages.end(), [](const std::weak_ptr<const DispatchVariant>& stage) { return stage.expired(); });
}

bool Driver::drawEntries() {
    static const bool entries = std::getenv("APS5_NO_DRAW_SRT_ENTRIES") == nullptr && !stampValidate();
    return entries;
}

bool Driver::drawCacheBypassed() {
    static const bool adaptive = std::getenv("APS5_NO_DRAW_CACHE_BYPASS") == nullptr;
    if (!adaptive) return false;
    auto left = drawCacheBypassLeft.load(std::memory_order_relaxed);
    while (left != 0) {
        if (drawCacheBypassLeft.compare_exchange_weak(left, left - 1, std::memory_order_relaxed)) {
            drawCacheBypassedDraws.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

void Driver::noteDrawCacheOutcome(bool hit) {
    constexpr std::uint32_t Window = 2048;
    constexpr std::uint32_t MinHitsPerWindow = Window / 20;
    constexpr std::uint32_t BypassDraws = 16384;
    if (hit) drawCacheProbeHits.fetch_add(1, std::memory_order_relaxed);
    if (drawCacheProbeLookups.fetch_add(1, std::memory_order_relaxed) + 1 < Window) return;
    const auto hits = drawCacheProbeHits.exchange(0, std::memory_order_relaxed);
    drawCacheProbeLookups.store(0, std::memory_order_relaxed);
    if (hits < MinHitsPerWindow) drawCacheBypassLeft.store(BypassDraws, std::memory_order_relaxed);
}

bool Driver::drawDataHits() {
    static const bool hits = std::getenv("APS5_NO_DRAW_DATA_HITS") == nullptr;
    return hits;
}

bool Driver::drawStageDataHits() {
    static const bool hits = std::getenv("APS5_NO_DRAW_STAGE_DATA_HITS") == nullptr && !stampValidate();
    return hits;
}

bool Driver::verifyDrawDataHits() {
    static const bool verify = std::getenv("APS5_VERIFY_DRAW_DATA_HITS") != nullptr;
    return verify;
}

bool Driver::verifyDrawEntries() {
    static const bool verify = std::getenv("APS5_VERIFY_DRAW_ENTRIES") != nullptr;
    return verify;
}

bool Driver::drawStampValidate() {
    static const bool enabled = std::getenv("APS5_NO_DRAW_STAMP_VALIDATE") == nullptr;
    return enabled;
}

bool Driver::registerKeyEnabled() {
    static const bool registerKey = std::getenv("APS5_NO_DRAW_KEY") == nullptr;
    return registerKey;
}

bool Driver::verifyDrawRecipe() {
    static const bool verify = std::getenv("APS5_VERIFY_DRAW_RECIPE") != nullptr;
    return verify;
}

std::size_t Driver::drawCacheEntries() {
    static const std::size_t entries = [] {
        const char* text = std::getenv("APS5_DRAW_CACHE_ENTRIES");
        const auto parsed = text != nullptr ? std::strtoull(text, nullptr, 10) : 0ull;
        return parsed != 0 ? static_cast<std::size_t>(parsed) : std::size_t{4096};
    }();
    return entries;
}

std::size_t Driver::drawVariants() {
    static const std::size_t variants = [] {
        const char* text = std::getenv("APS5_DRAW_VARIANTS");
        const auto parsed = text != nullptr ? std::strtoull(text, nullptr, 10) : 64ull;
        return static_cast<std::size_t>(std::clamp<unsigned long long>(parsed, 1, MaxDrawVariants));
    }();
    return variants;
}

void Driver::accountDrawVariant(const DispatchVariant& variant, bool added) {
    if (added) {
        ++drawCacheVariants;
        drawCacheVariantBytes += variantBytes(variant);
    } else {
        if (drawCacheVariants != 0) --drawCacheVariants;
        const auto bytes = variantBytes(variant);
        drawCacheVariantBytes = drawCacheVariantBytes > bytes ? drawCacheVariantBytes - bytes : 0;
    }
}

DrawKeySightings::DrawKeySightings(std::size_t keyCount) {
    Graphics::Require(keyCount >= Ways, "draw key sightings need at least one bucket");
    const auto buckets = std::bit_ceil(keyCount / Ways);
    keys.assign(buckets * Ways, 0);
    filled.assign(buckets, 0);
    next.assign(buckets, 0);
    bucketMask = buckets - 1;
}

bool DrawKeySightings::SeenBefore(std::uint64_t key) {
    auto mixed = key;
    mixed = (mixed ^ (mixed >> 30u)) * 0xbf58476d1ce4e5b9ull;
    mixed = (mixed ^ (mixed >> 27u)) * 0x94d049bb133111ebull;
    mixed ^= mixed >> 31u;
    const auto bucket = static_cast<std::size_t>(mixed) & bucketMask;
    auto* ways = keys.data() + bucket * Ways;
    if (std::find(ways, ways + filled[bucket], key) != ways + filled[bucket]) return true;
    ways[next[bucket]] = key;
    next[bucket] = static_cast<std::uint8_t>((next[bucket] + 1u) % Ways);
    if (filled[bucket] < Ways) ++filled[bucket];
    return false;
}

bool Driver::admitDrawKey(std::uint64_t key, std::uint64_t shape) {
    std::lock_guard cacheLock(drawCacheMutex);
    if (drawCache.contains(key)) return true;
    if (!drawKeySightings) drawKeySightings.emplace(drawCacheEntries() * DrawKeySightings::Ways);
    const bool seen = drawKeySightings->SeenBefore(key);
    return seen || (shape != 0 && !drawShapes.contains(shape));
}

void Driver::insertDrawEntry(std::uint64_t key, std::vector<std::shared_ptr<DispatchVariant>>& fresh, std::shared_ptr<const DrawDecode> decode, std::uint64_t shape) {
    std::lock_guard cacheLock(drawCacheMutex);
    auto& counters = drawEntryCounters;
    ++counters.inserts;
    const auto found = drawCache.find(key);
    auto replacement = std::make_shared<DrawEntry>();
    replacement->stages.resize(fresh.size());
    replacement->decode = std::move(decode);
    replacement->shape = shape;
    if (found != drawCache.end()) {
        if (found->second->stages.size() == fresh.size()) replacement->stages = found->second->stages;
        if (found->second->decode != nullptr) replacement->decode = found->second->decode;
        replacement->recipes.store(found->second->recipes.load());
    }
    for (std::size_t i = 0; i < fresh.size(); ++i) {
        if (fresh[i] == nullptr) continue;
        auto& variants = replacement->stages[i];
        const auto present = std::find_if(variants.begin(), variants.end(), [&](const std::shared_ptr<DispatchVariant>& kept) {
            return kept->pushOffset == fresh[i]->pushOffset && kept->userData == fresh[i]->userData && kept->runs == fresh[i]->runs && kept->words == fresh[i]->words;
        });
        if (present != variants.end()) {
            ++counters.present;
            fresh[i] = *present;
            if (present != variants.begin()) {
                auto matchedVariant = *present;
                variants.erase(present);
                variants.insert(variants.begin(), std::move(matchedVariant));
            }
            continue;
        }
        accountDrawVariant(*fresh[i], true);
        variants.insert(variants.begin(), fresh[i]);
        ++counters.variantsInserted;
        while (variants.size() > drawVariants()) {
            accountDrawVariant(*variants.back(), false);
            variants.pop_back();
            ++counters.variantsEvicted;
        }
    }
    replacement->touched = drawCacheHits;
    if (replacement->decode != nullptr && shape != 0) drawShapes[shape] = replacement;
    if (found == drawCache.end()) {
        drawOrder.push_front(key);
        replacement->order = drawOrder.begin();
        drawCache.emplace(key, replacement);
    } else {
        replacement->order = found->second->order;
        drawOrder.splice(drawOrder.begin(), drawOrder, replacement->order);
        found->second = replacement;
    }
    while (drawCache.size() > drawCacheEntries()) {
        const auto last = drawCache.find(drawOrder.back());
        if (traceDrawCache()) traceInsert(traceKeysEvicted, last->first);
        drawOrder.erase(last->second->order);
        drawCache.erase(last);
        ++drawCacheEvictions;
        ++counters.evictions;
    }
}

void Driver::eraseDrawEntry(std::uint64_t key, const std::shared_ptr<DrawEntry>& expected) {
    const auto found = drawCache.find(key);
    if (found == drawCache.end() || found->second != expected) return;
    for (const auto& variants : found->second->stages) {
        for (const auto& variant : variants) accountDrawVariant(*variant, false);
    }
    if (found->second->shape != 0) {
        const auto shapeOf = drawShapes.find(found->second->shape);
        if (shapeOf != drawShapes.end() && shapeOf->second == expected) drawShapes.erase(shapeOf);
    }
    drawOrder.erase(found->second->order);
    drawCache.erase(found);
}

bool DecodeProgramsCurrent(const DrawDecode& decode, const ShaderRegistry&) {
    return std::all_of(decode.programs.begin(), decode.programs.end(), [&](const DrawProgram& program) {
        if (!program.snapshot->header.empty()) return true;
        return ReadRawShader(program.snapshot->codeAddress + program.codeOffset * sizeof(std::uint32_t)) == program.snapshot &&
            (program.tailContinuation == nullptr || ReadRawShader(program.tailContinuation->codeAddress) == program.tailContinuation);
    });
}

std::shared_ptr<const DrawRecipe> Driver::findDrawRecipe(std::uint64_t key, const std::vector<std::shared_ptr<DispatchVariant>>& stages) {
    std::shared_ptr<const std::vector<DrawRecipeRecord>> records;
    {
        std::lock_guard cacheLock(drawCacheMutex);
        const auto found = drawCache.find(key);
        if (found == drawCache.end()) return nullptr;
        records = found->second->recipes.load();
    }
    if (records == nullptr) return nullptr;
    for (const auto& record : *records) {
        if (record.Matches(stages)) return record.recipe;
    }
    return nullptr;
}

void Driver::attachDrawRecipe(std::uint64_t key, const std::vector<std::shared_ptr<DispatchVariant>>& stages, std::shared_ptr<const DrawRecipe> recipe) {
    std::shared_ptr<DrawEntry> entry;
    {
        std::lock_guard cacheLock(drawCacheMutex);
        const auto found = drawCache.find(key);
        if (found == drawCache.end()) return;
        entry = found->second;
    }
    const auto old = entry->recipes.load();
    auto records = std::make_shared<std::vector<DrawRecipeRecord>>();
    records->push_back({std::vector<std::weak_ptr<const DispatchVariant>>(stages.begin(), stages.end()), std::move(recipe)});
    if (old != nullptr) {
        for (const auto& record : *old) {
            if (record.Matches(stages) || record.Expired() || records->size() >= dispatchVariants()) continue;
            records->push_back(record);
        }
    }
    entry->recipes.store(std::move(records));
    VulkanDevice::NoteRecipe(VulkanDevice::RecipeEvent::Attach, VulkanDevice::RecipeKind::Draw);
}

}
