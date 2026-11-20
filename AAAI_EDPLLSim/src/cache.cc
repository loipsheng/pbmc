#include "cache.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <utility>

#include "linear_constraint.h"

// ---------------------------------------------------------------------------
// Shared <= row normalization

Int floorDiv(const Int &numerator, const Int &positive_denominator) {
  assert(positive_denominator.cmp(0) > 0);
  Int quotient;
  mpz_fdiv_q(quotient.a, numerator.a, positive_denominator.a);
  return quotient;
}

LeqRowNormalizationResult NormalizeLeqRow(const std::vector<Int> &row) {
  assert(!row.empty());

  LeqRowNormalizationResult result;
  result.row = row;

  const std::size_t coefficient_count = row.size() - 1;
  Int coefficient_gcd(0);
  for (std::size_t i = 0; i < coefficient_count; ++i)
    Int::gcd(coefficient_gcd, coefficient_gcd, row[i]);

  if (coefficient_gcd == 0) {
    result.status = row.back().cmp(0) >= 0
        ? LeqRowStatus::kTautology
        : LeqRowStatus::kContradiction;
    result.changed = true;
    return result;
  }

  if (coefficient_gcd.cmp(1) > 0) {
    for (std::size_t i = 0; i < coefficient_count; ++i)
      result.row[i] /= coefficient_gcd;
    result.row.back() = floorDiv(row.back(), coefficient_gcd);
    result.changed = true;
  }

  return result;
}

// ---------------------------------------------------------------------------
// Canonical component representation and key construction

namespace {

std::size_t Uleb128Size(std::uint64_t value) {
  std::size_t size = 1;
  while (value >= 0x80U) {
    value >>= 7;
    ++size;
  }
  return size;
}

std::size_t SerializedBigIntSize(const Int &value) {
  if (mpz_sgn(value.a) == 0)
    return 1;
  const std::size_t magnitude_size =
      (mpz_sizeinbase(value.a, 2) + 7) / 8;
  return 1 + Uleb128Size(magnitude_size) + magnitude_size;
}

std::size_t SerializedRowTemplateSize(
    const std::vector<NormalizedTerm> &terms) {
  std::size_t size = Uleb128Size(terms.size());
  std::uint32_t previous_local_var = 0;
  bool first = true;
  for (const NormalizedTerm &term : terms) {
    assert(first || term.local_var > previous_local_var);
    const std::uint32_t encoded_local_var = first
        ? term.local_var
        : term.local_var - previous_local_var;
    size += Uleb128Size(encoded_local_var) +
        SerializedBigIntSize(term.coefficient);
    previous_local_var = term.local_var;
    first = false;
  }
  return size;
}

struct RowTemplateReference {
  RowTemplateID id = 0;
  Int rhs;
};

bool RowTemplateReferenceLess(const RowTemplateReference &left,
                              const RowTemplateReference &right) {
  if (left.id != right.id)
    return left.id < right.id;
  return left.rhs < right.rhs;
}

std::size_t SerializedComponentKeySize(
    const std::vector<std::pair<Int, Int>> &domains,
    const std::vector<RowTemplateReference> &rows) {
  std::size_t size = Uleb128Size(kComponentKeyFormatVersion) +
      Uleb128Size(rows.size());
  RowTemplateID previous = 0;
  for (const RowTemplateReference &row : rows) {
    assert(row.id >= previous);
    size += Uleb128Size(row.id - previous);
    previous = row.id;
  }
  size += Uleb128Size(domains.size());
  for (const auto &domain : domains)
    size += SerializedBigIntSize(domain.first) +
        SerializedBigIntSize(domain.second);
  for (const RowTemplateReference &row : rows)
    size += SerializedBigIntSize(row.rhs);
  return size;
}

SerializedComponentKey SerializeComponentKey(
    const std::vector<std::pair<Int, Int>> &domains,
    const std::vector<RowTemplateReference> &rows) {
  SerializedComponentKey serialized;
  serialized.bytes.reserve(SerializedComponentKeySize(domains, rows));
  AppendUleb128(kComponentKeyFormatVersion, serialized.bytes);
  AppendUleb128(static_cast<std::uint64_t>(rows.size()), serialized.bytes);
  RowTemplateID previous = 0;
  for (const RowTemplateReference &row : rows) {
    assert(row.id >= previous);
    AppendUleb128(row.id - previous, serialized.bytes);
    previous = row.id;
  }
  AppendUleb128(static_cast<std::uint64_t>(domains.size()), serialized.bytes);
  for (const auto &domain : domains) {
    AppendBigInt(domain.first, serialized.bytes);
    AppendBigInt(domain.second, serialized.bytes);
  }
  for (const RowTemplateReference &row : rows)
    AppendBigInt(row.rhs, serialized.bytes);
  serialized.hash = HashSerializedBytes(serialized.bytes);
  return serialized;
}

ComponentKeyBuildResult BuildComponentKey(
    const LinearConstraint &state, RowTemplatePool &pool,
    bool acquire) {
  NormalizedComponentBuildResult logical = ComponentNormalizer::normalize(state);
  ComponentKeyBuildResult result;
  result.status = logical.status;
  if (logical.status == CacheKeyBuildStatus::kContradiction ||
      logical.status == CacheKeyBuildStatus::kInvalid) {
    return result;
  }

  std::vector<RowTemplateReference> rows;
  rows.reserve(logical.key.rows.size());
  for (NormalizedRow &row : logical.key.rows) {
    SerializedComponentKey structure = SerializeRowTemplate(row.terms);
    RowTemplateID id = 0;
    if (acquire) {
      id = pool.internAndAcquire(std::move(structure));
    } else if (!pool.probe(structure, id)) {
      result.template_missing = true;
      return result;
    }
    RowTemplateReference reference;
    reference.id = id;
    reference.rhs = std::move(row.rhs);
    rows.emplace_back(std::move(reference));
  }
  std::sort(rows.begin(), rows.end(), RowTemplateReferenceLess);
  result.key = SerializeComponentKey(logical.key.domains, rows);
  return result;
}

}  // namespace

bool NormalizedTerm::operator==(const NormalizedTerm &other) const {
  return local_var == other.local_var && coefficient == other.coefficient;
}

bool NormalizedTerm::operator<(const NormalizedTerm &other) const {
  if (local_var != other.local_var)
    return local_var < other.local_var;
  return coefficient < other.coefficient;
}

bool NormalizedRow::operator==(const NormalizedRow &other) const {
  return terms == other.terms && rhs == other.rhs;
}

bool NormalizedRow::operator<(const NormalizedRow &other) const {
  if (terms != other.terms)
    return terms < other.terms;
  return rhs < other.rhs;
}

bool NormalizedComponent::operator==(
    const NormalizedComponent &other) const {
  return domains == other.domains && rows == other.rows;
}

bool SerializedComponentKey::operator==(const SerializedComponentKey &other) const {
  return bytes == other.bytes;
}

void AppendUleb128(std::uint64_t value, std::vector<std::uint8_t> &output) {
  do {
    std::uint8_t byte = static_cast<std::uint8_t>(value & 0x7fU);
    value >>= 7;
    if (value != 0)
      byte |= 0x80U;
    output.push_back(byte);
  } while (value != 0);
}

void AppendBigInt(const Int &value,
                  std::vector<std::uint8_t> &output) {
  const int sign = mpz_sgn(value.a);
  if (sign == 0) {
    output.push_back(0);
    return;
  }

  output.push_back(sign > 0 ? 1 : 2);
  const std::size_t magnitude_size =
      (mpz_sizeinbase(value.a, 2) + 7) / 8;
  AppendUleb128(static_cast<std::uint64_t>(magnitude_size), output);

  const std::size_t offset = output.size();
  output.resize(offset + magnitude_size);
  std::size_t written = 0;
  mpz_export(output.data() + offset, &written, 1, 1, 1, 0, value.a);
  assert(written == magnitude_size);
}

std::uint64_t HashSerializedBytes(const std::vector<std::uint8_t> &bytes) {
  // Stable 64-bit FNV-1a. This is only a bucket locator; equality compares
  // the complete byte vector.
  std::uint64_t hash = 14695981039346656037ULL;
  for (const std::uint8_t byte : bytes) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

SerializedComponentKey SerializeRowTemplate(
    const std::vector<NormalizedTerm> &terms) {
  SerializedComponentKey serialized;
  serialized.bytes.reserve(SerializedRowTemplateSize(terms));
  AppendUleb128(static_cast<std::uint64_t>(terms.size()), serialized.bytes);
  std::uint32_t previous_local_var = 0;
  bool first = true;
  for (const NormalizedTerm &term : terms) {
    assert(first || term.local_var > previous_local_var);
    const std::uint32_t encoded_local_var = first
        ? term.local_var
        : term.local_var - previous_local_var;
    AppendUleb128(encoded_local_var, serialized.bytes);
    AppendBigInt(term.coefficient, serialized.bytes);
    previous_local_var = term.local_var;
    first = false;
  }
  serialized.hash = HashSerializedBytes(serialized.bytes);
  return serialized;
}

NormalizedComponentBuildResult ComponentNormalizer::normalize(const LinearConstraint &state) {
  NormalizedComponentBuildResult result;
  result.key.domains = state.ranges;
  result.key.rows.reserve(state.matrix.size());

  const std::size_t variable_count = state.ranges.size();
  if (variable_count > std::numeric_limits<std::uint32_t>::max()) {
    result.status = CacheKeyBuildStatus::kInvalid;
    return result;
  }

  for (const std::vector<Int> &input_row : state.matrix) {
    assert(input_row.size() == variable_count + 1);
    if (input_row.size() != variable_count + 1) {
      result.status = CacheKeyBuildStatus::kInvalid;
      return result;
    }

    LeqRowNormalizationResult normalized = NormalizeLeqRow(input_row);
    if (normalized.status == LeqRowStatus::kTautology)
      continue;
    if (normalized.status == LeqRowStatus::kContradiction) {
      result.status = CacheKeyBuildStatus::kContradiction;
      result.key.rows.clear();
      return result;
    }

    NormalizedRow row;
    row.terms.reserve(variable_count);
    for (std::size_t local_var = 0; local_var < variable_count; ++local_var) {
      if (normalized.row[local_var] == 0)
        continue;
      row.terms.push_back(NormalizedTerm{
          static_cast<std::uint32_t>(local_var),
          normalized.row[local_var],
      });
    }
    row.rhs = normalized.row.back();
    result.key.rows.emplace_back(std::move(row));
  }

  std::sort(result.key.rows.begin(), result.key.rows.end());

  // Rows with equal primitive LHS are adjacent and ordered by RHS, so the
  // first one is the tightest constraint and all later ones are redundant.
  std::vector<NormalizedRow> unique_rows;
  unique_rows.reserve(result.key.rows.size());
  for (NormalizedRow &row : result.key.rows) {
    if (!unique_rows.empty() && unique_rows.back().terms == row.terms)
      continue;
    unique_rows.emplace_back(std::move(row));
  }
  result.key.rows.swap(unique_rows);

  result.status = result.key.rows.empty()
      ? CacheKeyBuildStatus::kTautologicalSystem
      : CacheKeyBuildStatus::kNormal;
  return result;
}

ComponentKeyBuildResult ComponentKeyBuilder::buildForLookup(
    const LinearConstraint &state, RowTemplatePool &pool) {
  return BuildComponentKey(state, pool, false);
}

ComponentKeyBuildResult ComponentKeyBuilder::buildForInsert(
    const LinearConstraint &state, RowTemplatePool &pool) {
  return BuildComponentKey(state, pool, true);
}

// ---------------------------------------------------------------------------
// Row-template storage and lifetime management

namespace {

constexpr std::size_t kMaximumTemplateBuckets = 1U << 18;

std::size_t FloorPowerOfTwo(std::size_t value) {
  if (value == 0)
    return 1;
  std::size_t result = 1;
  while (result <= value / 2)
    result *= 2;
  return result;
}

bool ReadUleb128(const std::vector<std::uint8_t> &bytes,
                 std::size_t &offset, std::uint64_t &value) {
  value = 0;
  unsigned shift = 0;
  for (unsigned count = 0; count < 10 && offset < bytes.size(); ++count) {
    const std::uint8_t byte = bytes[offset++];
    const std::uint64_t payload = byte & 0x7fU;
    if (shift == 63 && payload > 1)
      return false;
    value |= payload << shift;
    if ((byte & 0x80U) == 0)
      return true;
    shift += 7;
  }
  return false;
}

}  // namespace

RowTemplatePool::Record::Record(RowTemplateID record_id,
                                SerializedComponentKey serialized,
                                Record *next_record)
    : id(record_id), structure(std::move(serialized)), next(next_record) {}

RowTemplatePool::RowTemplatePool() { configure(1); }

RowTemplatePool::~RowTemplatePool() { deleteAllRecords(); }

void RowTemplatePool::configure(std::size_t initial_bucket_count) {
  deleteAllRecords();
  const std::size_t buckets = FloorPowerOfTwo(
      std::max<std::size_t>(1, initial_bucket_count));
  std::vector<Record *> fresh_buckets(buckets, nullptr);
  std::vector<Record *> fresh_ids(1, nullptr);  // ID zero is invalid.
  std::vector<RowTemplateID> fresh_free_ids;
  buckets_.swap(fresh_buckets);
  by_id_.swap(fresh_ids);
  free_ids_.swap(fresh_free_ids);
  zero_ref_head_ = nullptr;
  unique_current_ = 0;
  stats_ = Statistics();
}

void RowTemplatePool::deleteAllRecords() {
  for (Record *head : buckets_) {
    while (head != nullptr) {
      Record *next = head->next;
      delete head;
      head = next;
    }
  }
  zero_ref_head_ = nullptr;
}

void RowTemplatePool::clear() { configure(1); }

void RowTemplatePool::resetStatistics() {
  stats_ = Statistics();
}

std::size_t RowTemplatePool::bucketIndex(
    const SerializedComponentKey &structure) const {
  return static_cast<std::size_t>(structure.hash % buckets_.size());
}

bool RowTemplatePool::probe(const SerializedComponentKey &structure,
                                 RowTemplateID &id) {
  for (Record *record = buckets_[bucketIndex(structure)]; record != nullptr;
       record = record->next) {
    if (record->structure.hash == structure.hash) {
      if (record->structure == structure) {
        id = record->id;
        return true;
      }
    }
  }
  return false;
}

RowTemplateID RowTemplatePool::allocateID() {
  if (!free_ids_.empty()) {
    const RowTemplateID id = free_ids_.back();
    free_ids_.pop_back();
    assert(id < by_id_.size() && by_id_[id] == nullptr);
    return id;
  }
  assert(by_id_.size() <=
         static_cast<std::size_t>(
             std::numeric_limits<RowTemplateID>::max()));
  const RowTemplateID id =
      static_cast<RowTemplateID>(by_id_.size());
  by_id_.push_back(nullptr);
  return id;
}

void RowTemplatePool::maybeGrowBuckets() {
  if (unique_current_ + 1 <= 2 * buckets_.size() ||
      buckets_.size() >= kMaximumTemplateBuckets) {
    return;
  }
  const std::size_t new_count = std::min(
      buckets_.size() * 2, kMaximumTemplateBuckets);
  std::vector<Record *> fresh(new_count, nullptr);
  for (Record *head : buckets_) {
    while (head != nullptr) {
      Record *next = head->next;
      const std::size_t bucket =
          static_cast<std::size_t>(head->structure.hash % new_count);
      head->next = fresh[bucket];
      fresh[bucket] = head;
      head = next;
    }
  }
  buckets_.swap(fresh);
}

RowTemplateID RowTemplatePool::internAndAcquire(
    SerializedComponentKey structure) {
  ++stats_.intern_requests;
  const std::size_t initial_bucket = bucketIndex(structure);
  for (Record *record = buckets_[initial_bucket]; record != nullptr;
       record = record->next) {
    if (record->structure.hash == structure.hash) {
      if (record->structure == structure) {
        ++record->refcount;
        ++stats_.intern_reuses;
        return record->id;
      }
    }
  }

  maybeGrowBuckets();
  const RowTemplateID id = allocateID();
  const std::size_t bucket = bucketIndex(structure);
  Record *record = new Record(id, std::move(structure), buckets_[bucket]);
  record->refcount = 1;
  buckets_[bucket] = record;
  by_id_[id] = record;
  ++unique_current_;
  return id;
}

bool RowTemplatePool::validateTemplateIDs(
    const SerializedComponentKey &key, const RowTemplatePool &pool) {
  std::size_t offset = 0;
  std::uint64_t version = 0;
  std::uint64_t row_count = 0;
  if (!ReadUleb128(key.bytes, offset, version) ||
      version != kComponentKeyFormatVersion ||
      !ReadUleb128(key.bytes, offset, row_count)) {
    return false;
  }

  RowTemplateID previous = 0;
  for (std::uint64_t row = 0; row < row_count; ++row) {
    std::uint64_t delta = 0;
    if (!ReadUleb128(key.bytes, offset, delta) ||
        delta > std::numeric_limits<RowTemplateID>::max() - previous) {
      return false;
    }
    const RowTemplateID id = previous + delta;
    if (id == 0 || id >= pool.by_id_.size() || pool.by_id_[id] == nullptr)
      return false;
    previous = id;
  }
  return true;
}

bool RowTemplatePool::releaseKeyReferences(
    const SerializedComponentKey &key) {
  const bool ok = validateTemplateIDs(key, *this);
  if (!ok)
    return false;

  // IDs in a generated key are sorted. Validate each run's multiplicity
  // before changing any refcount, so even a malformed external key cannot
  // cause a partial release.
  std::size_t validation_offset = 0;
  std::uint64_t validation_version = 0;
  std::uint64_t validation_rows = 0;
  if (!ReadUleb128(key.bytes, validation_offset, validation_version) ||
      validation_version != kComponentKeyFormatVersion ||
      !ReadUleb128(key.bytes, validation_offset, validation_rows)) {
    return false;
  }
  RowTemplateID validation_previous = 0;
  RowTemplateID run_id = 0;
  std::uint64_t run_length = 0;
  for (std::uint64_t row = 0; row < validation_rows; ++row) {
    std::uint64_t delta = 0;
    if (!ReadUleb128(key.bytes, validation_offset, delta) ||
        delta > std::numeric_limits<RowTemplateID>::max() -
            validation_previous) {
      return false;
    }
    const RowTemplateID id = validation_previous + delta;
    if (id != run_id) {
      if (run_id != 0 && by_id_[run_id]->refcount < run_length)
        return false;
      run_id = id;
      run_length = 1;
    } else {
      ++run_length;
    }
    validation_previous = id;
  }
  if (run_id != 0 && by_id_[run_id]->refcount < run_length)
    return false;

  // Validation is deliberately separate from mutation: a malformed key can
  // never leave only part of its references released.
  std::size_t offset = 0;
  std::uint64_t version = 0;
  std::uint64_t row_count = 0;
  const bool header_ok = ReadUleb128(key.bytes, offset, version) &&
      ReadUleb128(key.bytes, offset, row_count);
  assert(header_ok && version == kComponentKeyFormatVersion);
  RowTemplateID previous = 0;
  for (std::uint64_t row = 0; row < row_count; ++row) {
    std::uint64_t delta = 0;
    const bool id_ok = ReadUleb128(key.bytes, offset, delta);
    assert(id_ok &&
           delta <= std::numeric_limits<RowTemplateID>::max() -
               previous);
    const RowTemplateID id = previous + delta;
    Record *record = by_id_[id];
    assert(record != nullptr && record->refcount != 0);
    --record->refcount;
    if (record->refcount == 0 && !record->queued_as_zero) {
      record->queued_as_zero = true;
      record->zero_next = zero_ref_head_;
      zero_ref_head_ = record;
    }
    previous = id;
  }
  return true;
}

std::uint64_t RowTemplatePool::reclaimUnreferenced() {
  std::uint64_t reclaimed = 0;
  while (zero_ref_head_ != nullptr) {
    Record *removed = zero_ref_head_;
    zero_ref_head_ = removed->zero_next;
    removed->zero_next = nullptr;
    removed->queued_as_zero = false;
    // A retained zero-reference template may have been reused since it was
    // queued. In that case it is no longer reclaimable.
    if (removed->refcount != 0)
      continue;

    const std::size_t bucket = bucketIndex(removed->structure);
    Record *current = buckets_[bucket];
    Record *previous = nullptr;
    while (current != removed) {
      assert(current != nullptr);
      previous = current;
      current = current->next;
    }
    if (previous == nullptr)
      buckets_[bucket] = removed->next;
    else
      previous->next = removed->next;
    by_id_[removed->id] = nullptr;
    free_ids_.push_back(removed->id);
    --unique_current_;
    ++reclaimed;
    delete removed;
  }

  if (reclaimed != 0) {
    while (by_id_.size() > 1 && by_id_.back() == nullptr)
      by_id_.pop_back();
    free_ids_.erase(
        std::remove_if(free_ids_.begin(), free_ids_.end(),
                       [this](RowTemplateID id) {
                         return id >= by_id_.size();
                       }),
        free_ids_.end());

    std::size_t desired_buckets = buckets_.size();
    while (desired_buckets > 1 &&
           unique_current_ <= desired_buckets / 2)
      desired_buckets /= 2;
    if (desired_buckets != buckets_.size()) {
      std::vector<Record *> compact_buckets(desired_buckets, nullptr);
      for (Record *head : buckets_) {
        while (head != nullptr) {
          Record *next = head->next;
          const std::size_t bucket = static_cast<std::size_t>(
              head->structure.hash % desired_buckets);
          head->next = compact_buckets[bucket];
          compact_buckets[bucket] = head;
          head = next;
        }
      }
      buckets_.swap(compact_buckets);
    }

    std::vector<Record *>(by_id_).swap(by_id_);
    std::vector<RowTemplateID>(free_ids_).swap(free_ids_);
  }
  return reclaimed;
}

std::size_t RowTemplatePool::size() const {
  return static_cast<std::size_t>(unique_current_);
}

double RowTemplatePool::reuseRate() const {
  if (stats_.intern_requests == 0)
    return 0.0;
  return static_cast<double>(stats_.intern_reuses) /
      static_cast<double>(stats_.intern_requests);
}

const RowTemplatePool::Statistics &
RowTemplatePool::statistics() const {
  return stats_;
}

// ---------------------------------------------------------------------------
// Component-cache lookup, insertion-age management, and eviction

namespace {

constexpr std::size_t kMaximumAutomaticBuckets = 1U << 18;

std::uint64_t SaturatingMultiply(std::uint64_t left,
                                 std::uint64_t right) {
  if (left != 0 &&
      right > std::numeric_limits<std::uint64_t>::max() / left)
    return std::numeric_limits<std::uint64_t>::max();
  return left * right;
}

}  // namespace

ComponentCache::Entry::Entry(SerializedComponentKey entry_key,
                             const Int &entry_value,
                             std::uint64_t sequence)
    : key(std::move(entry_key)), value(entry_value),
      created_sequence(sequence) {}

ComponentCache::ComponentCache() {
  Config config;
  config.bucket_count = 1;
  configure(config);
}

ComponentCache::ComponentCache(const Config &config) {
  configure(config);
}

ComponentCache::~ComponentCache() { deleteAllEntries(); }

std::size_t ComponentCache::chooseBucketCount(const Config &config) {
  std::size_t requested = config.bucket_count;
  if (requested == 0)
    requested = 1024;
  return std::max<std::size_t>(1, requested);
}

void ComponentCache::configure(
    const Config &config, RowTemplatePool *template_pool) {
  deleteAllEntries();
  if (template_pool_ != nullptr)
    template_pool_->reclaimUnreferenced();
  template_pool_ = template_pool;
  config_ = config;
  automatic_buckets_ = config.bucket_count == 0;
  std::vector<Entry *> fresh(chooseBucketCount(config_), nullptr);
  buckets_.swap(fresh);
  oldest_ = nullptr;
  newest_ = nullptr;
  insertion_sequence_ = 0;
  entries_current_ = 0;
  stats_ = Statistics();
}

std::size_t ComponentCache::maximumAutomaticBucketCount() const {
  if (!automatic_buckets_)
    return buckets_.size();
  return kMaximumAutomaticBuckets;
}

void ComponentCache::deleteAllEntries() {
  for (Entry *head : buckets_) {
    while (head != nullptr) {
      Entry *next = head->next;
      if (template_pool_ != nullptr) {
        const bool released = template_pool_->releaseKeyReferences(head->key);
        assert(released);
      }
      delete head;
      head = next;
    }
  }
  oldest_ = nullptr;
  newest_ = nullptr;
}

void ComponentCache::clear() {
  deleteAllEntries();
  if (template_pool_ != nullptr)
    template_pool_->reclaimUnreferenced();
  std::fill(buckets_.begin(), buckets_.end(), nullptr);
  insertion_sequence_ = 0;
  entries_current_ = 0;
  stats_ = Statistics();
}

void ComponentCache::resetStatistics() {
  stats_ = Statistics();
  stats_.entries_peak = entries_current_;
}

std::size_t ComponentCache::bucketIndex(
    const SerializedComponentKey &key) const {
  return static_cast<std::size_t>(key.hash % buckets_.size());
}

bool ComponentCache::isStale(const Entry &entry) const {
  return insertion_sequence_ - entry.created_sequence >
      config_.max_age_inserts;
}

void ComponentCache::updatePeaks() {
  stats_.entries_peak = std::max(stats_.entries_peak,
                                 entries_current_);
}

void ComponentCache::accountInsertion(const Entry &) {
  ++entries_current_;
  updatePeaks();
}

void ComponentCache::accountRemoval(const Entry &entry) {
  assert(entries_current_ > 0);
  --entries_current_;
  if (template_pool_ != nullptr) {
    const bool released = template_pool_->releaseKeyReferences(entry.key);
    assert(released);
  }
  ++stats_.deleted_entries;
}

void ComponentCache::releasePendingTemplateReferences(
    const SerializedComponentKey &key) {
  if (template_pool_ == nullptr)
    return;
  const bool released = template_pool_->releaseKeyReferences(key);
  assert(released);
}

void ComponentCache::unlinkBucket(Entry &entry) {
  const std::size_t bucket = bucketIndex(entry.key);
  if (entry.bucket_newer == nullptr) {
    assert(buckets_[bucket] == &entry);
    buckets_[bucket] = entry.next;
  } else {
    assert(entry.bucket_newer->next == &entry);
    entry.bucket_newer->next = entry.next;
  }
  if (entry.next != nullptr) {
    assert(entry.next->bucket_newer == &entry);
    entry.next->bucket_newer = entry.bucket_newer;
  }
  entry.next = nullptr;
  entry.bucket_newer = nullptr;
}

void ComponentCache::unlinkInsertionOrder(Entry &entry) {
  if (entry.older == nullptr)
    oldest_ = entry.newer;
  else
    entry.older->newer = entry.newer;
  if (entry.newer == nullptr)
    newest_ = entry.older;
  else
    entry.newer->older = entry.older;
  entry.older = nullptr;
  entry.newer = nullptr;
}

void ComponentCache::reclaimTemplatesAfterBatch() {
  if (template_pool_ == nullptr)
    return;
  template_pool_->reclaimUnreferenced();
}

std::uint64_t ComponentCache::removeSuffix(Entry *entry) {
  std::uint64_t removed = 0;
  while (entry != nullptr) {
    Entry *next = entry->next;
    entry->next = nullptr;
    entry->bucket_newer = nullptr;
    unlinkInsertionOrder(*entry);
    accountRemoval(*entry);
    delete entry;
    entry = next;
    ++removed;
  }
  return removed;
}

std::uint64_t ComponentCache::cleanStaleSuffix(std::size_t bucket) {
  Entry *current = buckets_[bucket];
  Entry *previous = nullptr;
  while (current != nullptr) {
    if (isStale(*current)) {
      if (previous == nullptr)
        buckets_[bucket] = nullptr;
      else
        previous->next = nullptr;
      current->bucket_newer = nullptr;
      const std::uint64_t removed = removeSuffix(current);
      reclaimTemplatesAfterBatch();
      return removed;
    }
    previous = current;
    current = current->next;
  }
  return 0;
}

void ComponentCache::evictOldestToCount(
    std::uint64_t desired_entries) {
  while (oldest_ != nullptr && entries_current_ > desired_entries) {
    Entry *entry = oldest_;
    unlinkBucket(*entry);
    unlinkInsertionOrder(*entry);
    accountRemoval(*entry);
    delete entry;
  }
}

void ComponentCache::maybeCleanByEntryLimit() {
  const std::uint64_t limit = cleanLimitEntries();
  // max_age_inserts=0 is useful for testing/immediate lazy expiry. Treat its
  // derived zero clean limit as disabling only the global count trigger.
  if (limit == 0 || entries_current_ < limit)
    return;

  ++stats_.cleanup_runs;
  const std::uint64_t keep =
      std::max<std::uint64_t>(1, entries_current_ / 4);
  evictOldestToCount(keep);
  reclaimTemplatesAfterBatch();
}

void ComponentCache::maybeGrowBuckets() {
  if (!automatic_buckets_ ||
      entries_current_ + 1 <= 2 * buckets_.size() ||
      buckets_.size() >= maximumAutomaticBucketCount()) {
    return;
  }

  const std::size_t new_count = std::min(
      buckets_.size() * 2, maximumAutomaticBucketCount());
  std::vector<Entry *> fresh(new_count, nullptr);

  // Walk oldest-to-newest and prepend. The result is newest-to-oldest in
  // every bucket, while both bucket directions are rebuilt in one pass.
  for (Entry *entry = oldest_; entry != nullptr; entry = entry->newer) {
    const std::size_t bucket =
        static_cast<std::size_t>(entry->key.hash % new_count);
    entry->bucket_newer = nullptr;
    entry->next = fresh[bucket];
    if (entry->next != nullptr)
      entry->next->bucket_newer = entry;
    fresh[bucket] = entry;
  }
  buckets_.swap(fresh);
}

bool ComponentCache::find(const SerializedComponentKey &key, Int &value) {
  const std::size_t bucket = bucketIndex(key);
  Entry *current = buckets_[bucket];
  Entry *previous = nullptr;
  while (current != nullptr) {
    if (current->key.hash == key.hash) {
      if (current->key == key) {
        value = current->value;
        return true;
      }
    }
    // Cachet checks equality first.  Consequently an old entry can still hit
    // when it is the queried entry; hits do not renew or reorder it.
    if (isStale(*current)) {
      if (previous == nullptr)
        buckets_[bucket] = nullptr;
      else
        previous->next = nullptr;
      current->bucket_newer = nullptr;
      removeSuffix(current);
      reclaimTemplatesAfterBatch();
      return false;
    }
    previous = current;
    current = current->next;
  }
  return false;
}

ComponentCache::InsertResult ComponentCache::insert(
    SerializedComponentKey key, const Int &value,
    bool template_references_acquired) {
  assert((template_pool_ != nullptr) == template_references_acquired);
  std::size_t bucket = bucketIndex(key);
  cleanStaleSuffix(bucket);

  for (Entry *entry = buckets_[bucket]; entry != nullptr;
       entry = entry->next) {
    if (entry->key.hash == key.hash) {
      if (entry->key == key) {
        // An exact component has one exact model count.  Updating does not
        // renew insertion age or move the node.
        assert(entry->value == value);
        releasePendingTemplateReferences(key);
        return InsertResult::kUpdated;
      }
    }
  }

  if (insertion_sequence_ ==
      std::numeric_limits<std::uint64_t>::max()) {
    // This is practically unreachable; clearing is safer than wrapping ages.
    clear();
    bucket = bucketIndex(key);
  }
  const std::uint64_t sequence = insertion_sequence_ + 1;
  Entry *entry = new Entry(std::move(key), value, sequence);
  maybeGrowBuckets();

  insertion_sequence_ = sequence;
  bucket = bucketIndex(entry->key);
  entry->next = buckets_[bucket];
  if (entry->next != nullptr)
    entry->next->bucket_newer = entry;
  buckets_[bucket] = entry;
  entry->older = newest_;
  if (newest_ == nullptr)
    oldest_ = entry;
  else
    newest_->newer = entry;
  newest_ = entry;
  accountInsertion(*entry);
  maybeCleanByEntryLimit();
  return InsertResult::kInserted;
}

std::size_t ComponentCache::size() const {
  return static_cast<std::size_t>(entries_current_);
}

std::uint64_t ComponentCache::cleanLimitEntries() const {
  return cleanLimitForAge(config_.max_age_inserts);
}

std::uint64_t ComponentCache::cleanLimitForAge(
    std::uint64_t max_age_inserts) {
  return SaturatingMultiply(max_age_inserts, 20);
}

const ComponentCache::Statistics &ComponentCache::statistics()
    const {
  return stats_;
}
