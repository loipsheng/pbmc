#ifndef EDPLLSIM_CACHE_H
#define EDPLLSIM_CACHE_H

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "big_int.h"

// Shared <= row normalization used by both RPR and cache-key construction.
enum class LeqRowStatus {
  kNormal,
  kTautology,
  kContradiction,
};

struct LeqRowNormalizationResult {
  LeqRowStatus status = LeqRowStatus::kNormal;
  std::vector<Int> row;
  bool changed = false;
};

// Mathematical floor division. The denominator must be strictly positive.
Int floorDiv(const Int &numerator, const Int &positive_denominator);

// Normalize a dense integer row [a_0, ..., a_(n-1), rhs] representing
// sum(a_i*x_i) <= rhs. Coefficient positions are preserved for solver use.
LeqRowNormalizationResult NormalizeLeqRow(const std::vector<Int> &row);

class LinearConstraint;
class RowTemplatePool;

// Canonical component representation and lossless key construction.
using RowTemplateID = std::uint64_t;

struct NormalizedTerm {
  std::uint32_t local_var = 0;
  Int coefficient;

  bool operator==(const NormalizedTerm &other) const;
  bool operator<(const NormalizedTerm &other) const;
};

struct NormalizedRow {
  std::vector<NormalizedTerm> terms;
  Int rhs;

  bool operator==(const NormalizedRow &other) const;
  bool operator<(const NormalizedRow &other) const;
};

struct NormalizedComponent {
  std::vector<std::pair<Int, Int>> domains;
  std::vector<NormalizedRow> rows;

  bool operator==(const NormalizedComponent &other) const;
};

struct SerializedComponentKey {
  std::vector<std::uint8_t> bytes;
  std::uint64_t hash = 0;

  bool operator==(const SerializedComponentKey &other) const;
};

enum class CacheKeyBuildStatus {
  kNormal,
  kTautologicalSystem,
  kContradiction,
  kInvalid,
};

struct NormalizedComponentBuildResult {
  CacheKeyBuildStatus status = CacheKeyBuildStatus::kNormal;
  NormalizedComponent key;
};

struct ComponentKeyBuildResult {
  CacheKeyBuildStatus status = CacheKeyBuildStatus::kNormal;
  bool template_missing = false;
  SerializedComponentKey key;
};

constexpr std::uint8_t kComponentKeyFormatVersion = 2;

void AppendUleb128(std::uint64_t value, std::vector<std::uint8_t> &output);
void AppendBigInt(const Int &value,
                  std::vector<std::uint8_t> &output);
std::uint64_t HashSerializedBytes(const std::vector<std::uint8_t> &bytes);
SerializedComponentKey SerializeRowTemplate(
    const std::vector<NormalizedTerm> &terms);

class ComponentNormalizer {
 public:
  static NormalizedComponentBuildResult normalize(
      const LinearConstraint &state);
};

class ComponentKeyBuilder {
 public:
  // Lookup never creates a row template.  template_missing means the exact
  // component key cannot already exist in the cache.
  static ComponentKeyBuildResult buildForLookup(
      const LinearConstraint &state, RowTemplatePool &pool);
  // Insert interns and acquires one reference for every row template.  The
  // cache manager either adopts those references or rolls them back.
  static ComponentKeyBuildResult buildForInsert(
      const LinearConstraint &state, RowTemplatePool &pool);
};

// Shared storage and lifetime management for normalized row structures.
// One per-instance content-addressed pool for GCD-normalized coefficient
// structures.  RHS values intentionally remain in component keys.
class RowTemplatePool {
 public:
  struct Statistics {
    std::uint64_t intern_requests = 0;
    std::uint64_t intern_reuses = 0;
  };

  RowTemplatePool();
  ~RowTemplatePool();

  RowTemplatePool(const RowTemplatePool &) = delete;
  RowTemplatePool &operator=(const RowTemplatePool &) = delete;

  void configure(std::size_t initial_bucket_count = 1024);
  void clear();
  void resetStatistics();

  bool probe(const SerializedComponentKey &structure,
             RowTemplateID &id);
  RowTemplateID internAndAcquire(SerializedComponentKey structure);

  // A component key starts with version, row count, and its
  // delta-coded template IDs, so references can be maintained without
  // storing a second ID vector in every cache entry.
  bool releaseKeyReferences(const SerializedComponentKey &key);

  // Removes only refcount-zero records. IDs are reused only after the old
  // record has no possible live cache-key reference.
  std::uint64_t reclaimUnreferenced();

  std::size_t size() const;
  double reuseRate() const;
  const Statistics &statistics() const;

 private:
  struct Record {
    RowTemplateID id = 0;
    SerializedComponentKey structure;
    std::uint64_t refcount = 0;
    Record *next = nullptr;
    Record *zero_next = nullptr;
    bool queued_as_zero = false;

    Record(RowTemplateID record_id, SerializedComponentKey serialized,
           Record *next_record);
  };

  std::vector<Record *> buckets_;
  std::vector<Record *> by_id_;
  std::vector<RowTemplateID> free_ids_;
  Record *zero_ref_head_ = nullptr;
  Statistics stats_;
  std::uint64_t unique_current_ = 0;
  std::size_t bucketIndex(const SerializedComponentKey &structure) const;
  RowTemplateID allocateID();
  void maybeGrowBuckets();
  static bool validateTemplateIDs(const SerializedComponentKey &key,
                                  const RowTemplatePool &pool);
  void deleteAllRecords();
};

// Exact component-key lookup, insertion-age management, and eviction.
// Entry-aged separate-chaining cache for serialized, row-templated component
// keys. Entries in every bucket are ordered from newest to oldest by insertion
// sequence. A lookup never renews an entry's age.
class ComponentCache {
 public:
  struct Config {
    std::uint64_t max_age_inserts = 50ULL * 1024ULL;
    // Zero enables an automatically growing bucket directory.
    std::size_t bucket_count = 0;
  };

  struct Statistics {
    std::uint64_t entries_peak = 0;
    std::uint64_t cleanup_runs = 0;
    std::uint64_t deleted_entries = 0;
  };

  enum class InsertResult {
    kInserted,
    kUpdated,
  };

  ComponentCache();
  explicit ComponentCache(const Config &config);
  ~ComponentCache();

  ComponentCache(const ComponentCache &) = delete;
  ComponentCache &operator=(const ComponentCache &) = delete;

  // Reconfiguration starts a fresh per-instance cache.
  void configure(const Config &config,
                 RowTemplatePool *template_pool = nullptr);
  void clear();
  // Keeps entries and insertion ages, but restarts diagnostic counters.
  void resetStatistics();

  bool find(const SerializedComponentKey &key, Int &value);
  InsertResult insert(SerializedComponentKey key, const Int &value,
                      bool template_references_acquired = false);

  std::size_t size() const;
  std::uint64_t cleanLimitEntries() const;
  static std::uint64_t cleanLimitForAge(std::uint64_t max_age_inserts);
  const Statistics &statistics() const;

 private:
  struct Entry {
    SerializedComponentKey key;
    Int value;
    std::uint64_t created_sequence = 0;
    // Bucket chain: next is older and bucket_newer is newer. Keeping both
    // directions lets the global oldest list remove an entry from its bucket
    // in O(1), without searching that bucket again.
    Entry *next = nullptr;
    Entry *bucket_newer = nullptr;
    // Global insertion-order chain.
    Entry *older = nullptr;
    Entry *newer = nullptr;

    Entry(SerializedComponentKey entry_key, const Int &entry_value,
          std::uint64_t sequence);
  };

  Config config_;
  std::vector<Entry *> buckets_;
  Statistics stats_;
  std::uint64_t insertion_sequence_ = 0;
  std::uint64_t entries_current_ = 0;
  bool automatic_buckets_ = true;
  Entry *oldest_ = nullptr;
  Entry *newest_ = nullptr;
  RowTemplatePool *template_pool_ = nullptr;

  static std::size_t chooseBucketCount(const Config &config);
  std::size_t maximumAutomaticBucketCount() const;
  std::size_t bucketIndex(const SerializedComponentKey &key) const;
  bool isStale(const Entry &entry) const;
  void accountInsertion(const Entry &entry);
  void accountRemoval(const Entry &entry);
  void releasePendingTemplateReferences(const SerializedComponentKey &key);
  void unlinkBucket(Entry &entry);
  void unlinkInsertionOrder(Entry &entry);
  void updatePeaks();
  void reclaimTemplatesAfterBatch();
  std::uint64_t removeSuffix(Entry *entry);
  std::uint64_t cleanStaleSuffix(std::size_t bucket);
  void evictOldestToCount(std::uint64_t desired_entries);
  void maybeCleanByEntryLimit();
  void maybeGrowBuckets();
  void deleteAllEntries();
};

#endif
