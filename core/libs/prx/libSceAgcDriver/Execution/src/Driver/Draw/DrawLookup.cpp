#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <optional>

namespace AgcDriver::DriverDetail {

namespace {

// APS5_RELOCATED_DATA_HITS counters (10 s, printed under APS5_PROFILE_WAITS or APS5_TRACE_DRAW_CACHE).
std::atomic<std::uint64_t> refreshTries{0}, refreshHits{0}, relocateTries{0}, relocateEqual{0}, relocateData{0}, relocateDiffering{0}, relocateRefused{0};
// Differing moved stages: whether the differing words lie in runs that moved with a pointer or in fixed runs, and how
// many words differ (buckets 1, 2-4, 5-16, 17+).
std::atomic<std::uint64_t> differMovedOnly{0}, differFixedOnly{0}, differBoth{0}, differUnreadable{0}, differMovedWords{0}, differFixedWords{0};
std::array<std::atomic<std::uint64_t>, 4> differBuckets{};

void classifyMovedDiffering(const DispatchVariant& variant, std::span<const std::pair<std::uint64_t, std::uint64_t>> runsAt) {
    std::uint64_t moved = 0, fixed = 0;
    std::size_t offset = 0;
    std::vector<std::uint32_t> now;
    for (std::size_t r = 0; r < runsAt.size(); ++r) {
        const auto count = static_cast<std::size_t>((runsAt[r].second - runsAt[r].first) / sizeof(std::uint32_t));
        now.assign(count, 0u);
        if (GuestMemory::CopyMapped(runsAt[r].first, std::as_writable_bytes(std::span(now))) == GuestMemory::Compare::Unmapped) {
            differUnreadable.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const bool movedRun = runsAt[r].first != variant.runs[r].first;
        for (std::size_t k = 0; k < count && offset + k < variant.words.size(); ++k) {
            if (now[k] != variant.words[offset + k]) ++(movedRun ? moved : fixed);
        }
        offset += count;
    }
    (moved != 0 && fixed != 0 ? differBoth : moved != 0 ? differMovedOnly : differFixedOnly).fetch_add(1, std::memory_order_relaxed);
    differMovedWords.fetch_add(moved, std::memory_order_relaxed);
    differFixedWords.fetch_add(fixed, std::memory_order_relaxed);
    const auto total = moved + fixed;
    differBuckets[total <= 1 ? 0 : total <= 4 ? 1 : total <= 16 ? 2 : 3].fetch_add(1, std::memory_order_relaxed);
}

bool reportRelocatedHits() {
    static const bool report = std::getenv("APS5_PROFILE_WAITS") != nullptr || std::getenv("APS5_TRACE_DRAW_CACHE") != nullptr;
    static std::atomic<std::int64_t> next{0};
    if (!report) return false;
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto due = next.load(std::memory_order_relaxed);
    if (now < due || !next.compare_exchange_strong(due, now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::seconds(10)).count()) || due == 0) return false;
    const auto take = [](std::atomic<std::uint64_t>& counter) { return static_cast<unsigned long long>(counter.exchange(0, std::memory_order_relaxed)); };
    AgcDriver::ProfilePrint_nid_no_patch("[relocated-hits] (10 s) data refreshed at the same runs: %llu of %llu tried; moved runs: %llu tried, %llu equal, %llu with data refreshed, %llu differing, %llu not relocatable\n", take(refreshHits), take(refreshTries), take(relocateTries), take(relocateEqual), take(relocateData), take(relocateDiffering), take(relocateRefused));
    AgcDriver::ProfilePrint_nid_no_patch("[relocated-differ] (10 s) moved stages that differ: only in moved runs %llu, only in fixed runs %llu, both %llu, unreadable %llu; differing words in moved runs %llu, in fixed runs %llu; per stage 1: %llu, 2-4: %llu, 5-16: %llu, 17+: %llu\n", take(differMovedOnly), take(differFixedOnly), take(differBoth), take(differUnreadable), take(differMovedWords), take(differFixedWords), take(differBuckets[0]), take(differBuckets[1]), take(differBuckets[2]), take(differBuckets[3]));
    return true;
}

// The variant's runs moved by its changed base pointers, when every used user word that differs from the ones its runs
// were captured at is the base of one of its runs (tagVariantRuns), the stage binds no BDA page table, and no word past
// the tracked 64 changed. Nullopt otherwise.
std::optional<std::vector<std::pair<std::uint64_t, std::uint64_t>>> movedRuns(const DispatchVariant& variant, std::span<const std::uint32_t> now) {
    if (variant.relocatableWords == 0 || variant.userData.size() != now.size() || variant.runBases.size() != variant.runs.size() || variant.compiled == nullptr) return std::nullopt;
    if (std::any_of(variant.compiled->bindings.begin(), variant.compiled->bindings.end(), [](const ShaderRecompiler::DescriptorBinding& binding) { return binding.role == ShaderRecompiler::DescriptorRole::BdaPagetable; })) return std::nullopt;
    std::uint64_t changed = 0;
    for (std::size_t w = 0; w < now.size(); ++w) {
        if (now[w] == variant.userData[w]) continue;
        if (w >= 64u) return std::nullopt;
        if (((variant.userDataMask >> w) & 1u) != 0) changed |= std::uint64_t{1} << w;
    }
    if (changed == 0 || (changed & ~variant.relocatableWords) != 0) return std::nullopt;
    std::uint64_t bases = 0;
    for (const auto base : variant.runBases) {
        if (base >= 0) bases |= std::uint64_t{1} << base;
    }
    if ((changed & ~bases) != 0) return std::nullopt;
    auto runs = variant.runs;
    for (std::size_t r = 0; r < runs.size(); ++r) {
        const auto base = variant.runBases[r];
        if (base < 0 || ((changed >> base) & 1u) == 0) continue;
        const auto delta = static_cast<std::int64_t>(now[static_cast<std::size_t>(base)] & ~std::uint32_t{3}) - static_cast<std::int64_t>(variant.userData[static_cast<std::size_t>(base)] & ~std::uint32_t{3});
        runs[r].first = static_cast<std::uint64_t>(static_cast<std::int64_t>(runs[r].first) + delta);
        runs[r].second = static_cast<std::uint64_t>(static_cast<std::int64_t>(runs[r].second) + delta);
    }
    return runs;
}

void regionsAt(std::span<const std::pair<std::uint64_t, std::uint64_t>> runs, const std::vector<std::uint32_t>& words, std::vector<ShaderRecompiler::MemoryRegion>& regions) {
    regions.clear();
    regions.reserve(runs.size());
    std::size_t offset = 0;
    for (const auto& [begin, end] : runs) {
        const auto count = static_cast<std::size_t>((end - begin) / sizeof(std::uint32_t));
        regions.push_back({begin, std::as_bytes(std::span<const std::uint32_t>(words).subspan(offset, count))});
        offset += count;
    }
}

}

void Driver::lookupDraw(const Submission& submission, const std::shared_ptr<VulkanDevice>& localDevice, const Graphics::State& graphics, const ShaderRecompiler::ShaderPixelStageInfo& pixel, const std::vector<DrawProgram>& programs, const std::vector<ShaderRecompiler::ProgramRole>& roles, const std::vector<std::optional<ShaderRecompiler::ShaderVertexStageInfo>>& vertexInfos, bool useDrawEntries, bool registerKey, bool profile, std::uint64_t& drawKey, std::shared_ptr<DrawEntry>& entry, std::vector<std::shared_ptr<DispatchVariant>>& matched, std::vector<std::vector<ShaderRecompiler::MemoryRegion>>& matchedRegions, bool& drawHit, bool& verifyHit, DrawPhaseTiming& phaseTiming, std::array<double, DrawDriverPhaseCount>& phaseMs, const std::shared_ptr<DrawEntry>& dataEntry, DrawDataCandidates& dataCandidates, std::vector<StageDataHit>& stageData) {
    using Role = ShaderRecompiler::ProgramRole;
    if (useDrawEntries) {
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
                for (const auto value : {mesh.inputPrimitive, mesh.primitivesPerGroup, mesh.verticesPerGroup, mesh.maxVertices, mesh.maxPrimitives, mesh.threadsPerGroup, mesh.ldsSizeDwords, mesh.provokingVertex, mesh.esgsItemSize, mesh.outputPrimitiveSize}) mix(value);
            }
            mix(graphics.stages.tessellation.has_value());
            if (graphics.stages.tessellation) {
                const auto& tess = *graphics.stages.tessellation;
                for (const auto value : {tess.inputControlPoints, tess.outputControlPoints, tess.domain, tess.partitioning, tess.outputTopology, tess.patchesPerGroup, tess.primitiveGroupSize, static_cast<std::uint32_t>(tess.breakAtInstance)}) mix(value);
                mix(tess.mergedHull);
            }
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
                mix(static_cast<std::uint32_t>(vertex.drawBaseVertexSgpr));
                mix(static_cast<std::uint32_t>(vertex.drawStartInstanceSgpr));
                mix(vertex.pointList);
                mix(std::bit_cast<std::uint32_t>(vertex.pointSize));
                mix(std::bit_cast<std::uint32_t>(vertex.pointSizeMin));
                mix(std::bit_cast<std::uint32_t>(vertex.pointSizeMax));
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
            mix(pixel.stencilOperationValueExport);
            for (const auto value : pixel.targetOutputMode) mix(value);
            for (const auto value : pixel.targetExportMapping) mix(value);
            for (const auto value : pixel.targetExportPacking) mix(static_cast<std::uint64_t>(value));
            mix(pixel.dualSourceBlend);
            mix(pixel.reversedBlend);
            mix(pixel.targetMerge.target);
            mix(pixel.targetMerge.mask);
            mix(pixel.targetMerge.samples);
            for (const auto word : pixel.targetMerge.descriptor) mix(word);
            mix(pixel.targetMerge.depthStencilWrites);
            mix(pixel.targetMerge.depthStencilTests);
            mix(pixel.alphaToCoverage);
            for (const auto& blend : pixel.targetBlend) mix(static_cast<std::uint64_t>(blend.enable) | (static_cast<std::uint64_t>(blend.sourceFactor) << 8u) | (static_cast<std::uint64_t>(blend.destinationFactor) << 16u) | (static_cast<std::uint64_t>(blend.function) << 24u) | (static_cast<std::uint64_t>(blend.writeMask) << 32u));
            mix(static_cast<std::uint64_t>(pixel.rasterPrimitive));
            mix(pixel.ditheredAlphaToCoverage);
            mix(pixel.quadPixelMask);
            for (const auto mask : pixel.quadSampleMask) mix(mask);
            mix(pixel.iteratesSamples);
            mix(static_cast<std::uint64_t>(pixel.positionLocation));
            for (const auto location : pixel.sampleLocations) mix(location);
            for (const auto order : pixel.sampleOrder) mix(order);
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
                        if (programs[i].binary.stage == ShaderRecompiler::ShaderStage::Mesh && !std::equal(variant->meshIndexDescriptor.begin(), variant->meshIndexDescriptor.end(), programs[i].userData.begin() + ShaderRecompiler::MeshIndexBufferUserWord)) {
                            if (!anyLayout) outcome = EntryOutcome::Differing;
                            anyLayout = true;
                            continue;
                        }
                        auto& regions = matchedRegions[i];
                        regions.clear();
                        appendEntryRegions(*variant, regions);
                        ++compared;
                        auto result = validateVariant(programs[i].binary.codeAddress, submission.queue, *variant, regions, imagesFlushed, runsSynced, sampling, live);
                        if (gateRetry() && (result == EntryOutcome::PublishMoved || result == EntryOutcome::PendingMoved)) result = validateVariant(programs[i].binary.codeAddress, submission.queue, *variant, regions, imagesFlushed, runsSynced, sampling, live);
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
                        rotated->prepared = entry->prepared;
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
        } else if (dataEntry != nullptr && graphics.stages.path == Graphics::ShaderPath::Vertex && dataEntry->stages.size() == programs.size() && dataEntry->decode->programs.size() == programs.size()) {
            std::uint64_t imagesFlushed = 0, runsSynced = 0;
            const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::DrawCache);
            std::optional<SampledReadScope> sampling;
            // APS5_RELOCATED_DATA_HITS (runtime switch): a stage whose captured data words changed is refreshed into its
            // flat data binding (as a full hit's stage data), and a stage whose only changed used words are its
            // relocatable base pointers is validated at its moved runs; one such stage per program (its words live in
            // stageData[i]).
            const bool relocate = Graphics::RuntimeSwitches::Set(Graphics::RuntimeSwitch::RelocatedDataHits) && drawStageDataHits();
            std::vector<std::pair<std::uint32_t, std::uint32_t>> liveData;
            for (std::size_t i = 0; i < programs.size(); ++i) {
                if (roles[i] == Role::GeometryBack || programs[i].userData.size() != dataEntry->decode->programs[i].userData.size()) continue;
                const auto& now = programs[i].userData;
                const auto& was = dataEntry->decode->programs[i].userData;
                auto& candidates = dataCandidates[i];
                bool patched = false;
                for (const auto& variant : dataEntry->stages[i]) {
                    bool usedEqual = true;
                    for (std::size_t w = 0; w < now.size() && usedEqual; ++w) {
                        if (now[w] != was[w] && (w >= 64u || ((variant->userDataMask >> w) & 1u) != 0)) usedEqual = false;
                    }
                    if ((i == 0 && variant->pushOffset != 0) || std::any_of(candidates.begin(), candidates.end(), [&](const auto& candidate) { return candidate.first->pushOffset == variant->pushOffset; })) continue;
                    if (usedEqual) {
                        std::vector<ShaderRecompiler::MemoryRegion> regions;
                        appendEntryRegions(*variant, regions);
                        auto* live = relocate && !patched && !variant->dataPositions.empty() ? &liveData : nullptr;
                        auto result = validateVariant(programs[i].binary.codeAddress, submission.queue, *variant, regions, imagesFlushed, runsSynced, sampling, live);
                        if (gateRetry() && (result == EntryOutcome::PublishMoved || result == EntryOutcome::PendingMoved)) result = validateVariant(programs[i].binary.codeAddress, submission.queue, *variant, regions, imagesFlushed, runsSynced, sampling, live);
                        if (live != nullptr) refreshTries.fetch_add(1, std::memory_order_relaxed);
                        if (result == EntryOutcome::Equal) {
                            candidates.emplace_back(variant, std::move(regions));
                        } else if (result == EntryOutcome::EqualData && live != nullptr) {
                            auto& hit = stageData[i];
                            hit.words = variant->words;
                            for (const auto& [position, value] : liveData) hit.words[position] = value;
                            appendEntryRegions(*variant, regions = {}, &hit.words);
                            hit.compiled = patchDataWords(*variant, hit.words);
                            candidates.emplace_back(variant, std::move(regions));
                            patched = true;
                            refreshHits.fetch_add(1, std::memory_order_relaxed);
                        }
                        continue;
                    }
                    if (!relocate || patched) continue;
                    const auto runsAt = movedRuns(*variant, now);
                    if (!runsAt) {
                        if (variant->relocatableWords != 0) relocateRefused.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                    relocateTries.fetch_add(1, std::memory_order_relaxed);
                    std::vector<ShaderRecompiler::MemoryRegion> regions;
                    regionsAt(*runsAt, variant->words, regions);
                    auto* live = !variant->dataPositions.empty() ? &liveData : nullptr;
                    auto result = validateVariant(programs[i].binary.codeAddress, submission.queue, *variant, regions, imagesFlushed, runsSynced, sampling, live, *runsAt);
                    if (gateRetry() && (result == EntryOutcome::PublishMoved || result == EntryOutcome::PendingMoved)) result = validateVariant(programs[i].binary.codeAddress, submission.queue, *variant, regions, imagesFlushed, runsSynced, sampling, live, *runsAt);
                    if (result != EntryOutcome::Equal && result != EntryOutcome::EqualData) {
                        relocateDiffering.fetch_add(1, std::memory_order_relaxed);
                        if (result == EntryOutcome::Differing) classifyMovedDiffering(*variant, *runsAt);
                        continue;
                    }
                    auto& hit = stageData[i];
                    hit.words = variant->words;
                    if (result == EntryOutcome::EqualData) {
                        for (const auto& [position, value] : liveData) hit.words[position] = value;
                    }
                    regionsAt(*runsAt, hit.words, regions);
                    hit.compiled = result == EntryOutcome::EqualData ? patchDataWords(*variant, hit.words) : variant->compiled;
                    candidates.emplace_back(variant, std::move(regions));
                    patched = true;
                    (result == EntryOutcome::EqualData ? relocateData : relocateEqual).fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        if (reportRelocatedHits()) {
            std::lock_guard cacheLock(drawCacheMutex);
            reportDrawRelocation();
        }
        phaseTiming.Phase(DrawRowKeyLookupValidate);
    }
}

}
