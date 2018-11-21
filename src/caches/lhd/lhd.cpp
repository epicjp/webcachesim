#include <sstream>
#include <vector>
#include "cache.hpp"
#include "lhd.hpp"
#include "rand.hpp"

namespace repl {

LHD::LHD(cache::Cache* _cache)
    : cache(_cache) {
    nextReconfiguration = ACCS_PER_RECONFIGURATION;
    
    for (uint32_t i = 0; i < NUM_CLASSES; i++) {
        classes.push_back(Class());
        auto& cl = classes.back();
        cl.hits.resize(MAX_AGE, 0);
        cl.evictions.resize(MAX_AGE, 0);
        cl.hitDensities.resize(MAX_AGE, 0);
    }

    // Initialize policy to ~GDSF by default.
    for (uint32_t c = 0; c < NUM_CLASSES; c++) {
        for (age_t a = 0; a < MAX_AGE; a++) {
            classes[c].hitDensities[a] =
                1. * (c + 1) / (a + 1);
        }
    }
}

candidate_t LHD::rank(const parser::Request& req) {
    uint64_t victim = -1;
    rank_t victimRank = std::numeric_limits<rank_t>::max();

    // Sample a few candidates at first so we converge quickly to a
    // reasonable policy on this trace.
    uint32_t candidates =
        (numReconfigurations > 50)?
        ASSOCIATIVITY : 8;

    for (uint32_t i = 0; i < candidates; i++) {
        auto idx = rand.next() % tags.size();
        auto& tag = tags[idx];
        rank_t rank = getHitDensity(tag);

        if (rank < victimRank) {
            victim = idx;
            victimRank = rank;
        }
    }

    assert(victim != (uint64_t)-1);

    return tags[victim].id;
}

void LHD::update(candidate_t id, const parser::Request& req) {
    auto itr = indices.find(id);
    bool insert = (itr == indices.end());
        
    Tag* tag;
    if (insert) {
        tags.push_back(Tag{});
        tag = &tags.back();
        indices[id] = tags.size() - 1;
        
        tag->lastLastHitAge = MAX_AGE;
        tag->lastHitAge = 0;
        tag->id = id;
    } else {
        tag = &tags[itr->second];
        assert(tag->id == id);
        auto age = getAge(*tag);
        auto& cl = getClass(*tag);
        cl.hits[age] += 1;
        
        tag->lastLastHitAge = tag->lastHitAge;
        tag->lastHitAge = age;
    }

    age_t coarsenedTimestamp = timestamp >> ageCoarseningShift;
    tag->timestamp = coarsenedTimestamp;
    tag->app = req.appId % APP_CLASSES;
    tag->size = req.size();

    rand.next();

    ++timestamp;

    if (--nextReconfiguration == 0) {
        reconfigure();
        nextReconfiguration = ACCS_PER_RECONFIGURATION;
        ++numReconfigurations;
    }
}

void LHD::replaced(candidate_t id) {
    auto itr = indices.find(id);
    assert(itr != indices.end());
    auto index = itr->second;

    // Record stats before removing item
    auto& tag = tags[index];
    assert(tag.id == id);
    auto age = getAge(tag);
    auto& cl = getClass(tag);
    cl.evictions[age] += 1;

    // Remove tag for replaced item and update index
    indices.erase(itr);
    tags[index] = tags.back();
    tags.pop_back();

    if (index < tags.size()) {
        indices[tags[index].id] = index;
    }
}

void LHD::reconfigure() {
    rank_t totalHits = 0;
    rank_t totalEvictions = 0;
    for (auto& cl : classes) {
        updateClass(cl);
        totalHits += cl.totalHits;
        totalEvictions += cl.totalEvictions;
    }

    adaptAgeCoarsening();
        
    modelHitDensity();

    // Just printfs ...
    for (uint32_t c = 0; c < classes.size(); c++) {
        auto& cl = classes[c];
        // printf("Class %d | hits %g, evictions %g, hitRate %g\n",
        //        c,
        //        cl.totalHits, cl.totalEvictions,
        //        cl.totalHits / (cl.totalHits + cl.totalEvictions));

        dumpClassRanks(cl);
    }
    // printf("LHD | hits %g, evictions %g, hitRate %g | overflows %lu (%g) | cumulativeHitRate nan\n",
    //        totalHits, totalEvictions,
    //        totalHits / (totalHits + totalEvictions),
    //        overflows,
    //        1. * overflows / ACCS_PER_RECONFIGURATION);

    overflows = 0;
}

void LHD::updateClass(Class& cl) {
    cl.totalHits = 0;
    cl.totalEvictions = 0;

    for (age_t age = 0; age < MAX_AGE; age++) {
        cl.hits[age] *= EWMA_DECAY;
        cl.evictions[age] *= EWMA_DECAY;

        cl.totalHits += cl.hits[age];
        cl.totalEvictions += cl.evictions[age];
    }
}

void LHD::modelHitDensity() {
    for (uint32_t c = 0; c < classes.size(); c++) {
        rank_t totalEvents = classes[c].hits[MAX_AGE-1] + classes[c].evictions[MAX_AGE-1];
        rank_t totalHits = classes[c].hits[MAX_AGE-1];
        rank_t lifetimeUnconditioned = totalEvents;
        
        for (age_t a = MAX_AGE - 2; a < MAX_AGE; a--) {
            totalHits += classes[c].hits[a];
            
            totalEvents += classes[c].hits[a] + classes[c].evictions[a];

            lifetimeUnconditioned += totalEvents;

            if (totalEvents > 1e-5) {
                classes[c].hitDensities[a] = totalHits / lifetimeUnconditioned;
            } else {
                classes[c].hitDensities[a] = 0.;
            }
        }
    }
}

void LHD::dumpClassRanks(Class& cl) {
    if (!DUMP_RANKS) { return; }
    
    // float objectAvgSize = cl.sizeAccumulator / cl.totalHits; // + cl.totalEvictions);
    float objectAvgSize = 1. * cache->consumedCapacity / cache->getNumObjects();
    rank_t left;

    left = cl.totalHits + cl.totalEvictions;
    // std::cout << "Ranks for avg object (" << objectAvgSize << "): ";
    for (age_t a = 0; a < MAX_AGE; a++) {
      std::stringstream rankStr;
      rank_t density = cl.hitDensities[a] / objectAvgSize;
      rankStr << density << ", ";
      // std::cout << rankStr.str();

      left -= cl.hits[a] + cl.evictions[a];
      if (rankStr.str() == "0, " && left < 1e-2) {
        break;
      }
    }
    // std::cout << std::endl;

    left = cl.totalHits + cl.totalEvictions;
    // std::cout << "Hits: ";
    for (uint32_t a = 0; a < MAX_AGE; a++) {
      std::stringstream rankStr;
      rankStr << cl.hits[a] << ", ";
      // std::cout << rankStr.str();

      left -= cl.hits[a] + cl.evictions[a];
      if (rankStr.str() == "0, " && left < 1e-2) {
        break;
      }
    }
    // std::cout << std::endl;

    left = cl.totalHits + cl.totalEvictions;
    // std::cout << "Evictions: ";
    for (uint32_t a = 0; a < MAX_AGE; a++) {
      std::stringstream rankStr;
      rankStr << cl.evictions[a] << ", ";
      // std::cout << rankStr.str();

      left -= cl.hits[a] + cl.evictions[a];
      if (rankStr.str() == "0, " && left < 1e-2) {
        break;
      }
    }
    // std::cout << std::endl;
}

// this happens very rarely!
//
// it is simple enough to set the age coarsening if you know roughly
// how big your objects are. to make LHD run on different traces
// without needing to configure this, we set the age coarsening
// automatically near the beginning of the trace.
void LHD::adaptAgeCoarsening() {
    ewmaNumObjects *= EWMA_DECAY;
    ewmaNumObjectsMass *= EWMA_DECAY;

    ewmaNumObjects += cache->getNumObjects();
    ewmaNumObjectsMass += 1.;

    rank_t numObjects = ewmaNumObjects / ewmaNumObjectsMass;

    rank_t optimalAgeCoarsening = 1. * numObjects / (AGE_COARSENING_ERROR_TOLERANCE * MAX_AGE);

    // Simplify. Just do this once shortly after the trace starts and
    // again after 25 iterations. It only matters that we are within
    // the right order of magnitude to avoid tons of overflows.
    if (numReconfigurations == 5 || numReconfigurations == 25) {
        uint32_t optimalAgeCoarseningLog2 = 1;

        while ((1 << optimalAgeCoarseningLog2) < optimalAgeCoarsening) {
            optimalAgeCoarseningLog2 += 1;
        }

        int32_t delta = optimalAgeCoarseningLog2 - ageCoarseningShift;
        ageCoarseningShift = optimalAgeCoarseningLog2;

        // increase weight to delay another shift for a while
        ewmaNumObjects *= 8;
        ewmaNumObjectsMass *= 8;
        
        // compress or stretch distributions to approximate new scaling
        // regime
        if (delta < 0) {
            // stretch
            for (auto& cl : classes) {
                for (age_t a = MAX_AGE >> (-delta); a < MAX_AGE - 1; a++) {
                    cl.hits[MAX_AGE - 1] += cl.hits[a];
                    cl.evictions[MAX_AGE - 1] += cl.evictions[a];
                }
                for (age_t a = MAX_AGE - 2; a < MAX_AGE; a--) {
                    cl.hits[a] = cl.hits[a >> (-delta)] / (1 << (-delta));
                    cl.evictions[a] = cl.evictions[a >> (-delta)] / (1 << (-delta));
                }
            }
        } else if (delta > 0) {
            // compress
            for (auto& cl : classes) {
                for (age_t a = 0; a < MAX_AGE >> delta; a++) {
                    cl.hits[a] = cl.hits[a << delta];
                    cl.evictions[a] = cl.evictions[a << delta];
                    for (int i = 1; i < 1 << delta; i++) {
                        cl.hits[a] += cl.hits[(a << delta) + i];
                        cl.evictions[a] += cl.evictions[(a << delta) + i];
                    }
                }
                for (age_t a = MAX_AGE >> delta; a < MAX_AGE - 1; a++) {
                    cl.hits[a] = 0;
                    cl.evictions[a] = 0;
                }
            }
        }

        // adjust tags
        if (delta != 0) {
            for (auto& tag : tags) {
                tag.timestamp >>= delta;
            }
        }
    }
    
    // printf("LHD at %lu | ageCoarseningShift now %lu | num objects %g | optimal age coarsening %g | current age coarsening %g\n",
    //        timestamp, ageCoarseningShift,
    //        numObjects,
    //        optimalAgeCoarsening,
    //        1. * (1 << ageCoarseningShift));
}

} // namespace repl
