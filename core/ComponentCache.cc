/*
Copyright (c) 2025 Grabtaxi Holdings Pte Ltd (GRAB), All rights reserved.

Use of this source code is governed by an MIT-style license that can be found in the LICENSE file
*/

#include <iostream>
#include <algorithm>
#include <stdexcept>
#include <gmpxx.h>
#include "mpfr/mpreal.h"
#include "core/Component.h"

// #define USE_SYSINFO

#ifdef USE_SYSINFO
#include <sys/sysinfo.h>
#endif

using namespace GPMC;
using namespace std;

static uint64_t cacheHash(const vector<uint8_t>& bytes) {
	uint64_t hash = 14695981039346656037ULL;
	for (uint8_t byte : bytes) {
		hash ^= byte;
		hash *= 1099511628211ULL;
	}
	return hash;
}

static void appendCacheNumber(uint64_t value, vector<uint8_t>& output) {
	do {
		uint8_t byte = static_cast<uint8_t>(value & 0x7fU);
		value >>= 7;
		if (value != 0) byte |= 0x80U;
		output.push_back(byte);
	} while (value != 0);
}

static uint64_t readCacheNumber(
		const vector<uint8_t>& input, size_t& position, size_t end) {
	uint64_t result = 0;
	unsigned shift = 0;
	while (position < end && shift < 64) {
		const uint8_t byte = input[position++];
		result |= static_cast<uint64_t>(byte & 0x7fU) << shift;
		if ((byte & 0x80U) == 0) return result;
		shift += 7;
	}
	throw logic_error("invalid residual cache integer");
}

static void skipCacheBig(
		const vector<uint8_t>& input, size_t& position, size_t end) {
	if (position >= end) throw logic_error("missing residual cache bigint");
	const uint8_t sign = input[position++];
	if (sign == 0) return;
	if (sign != 1 && sign != 2)
		throw logic_error("invalid residual cache bigint sign");
	const uint64_t length = readCacheNumber(input, position, end);
	if (length > end - position)
		throw logic_error("truncated residual cache bigint");
	position += static_cast<size_t>(length);
}

static ResidualRowKey splitResidualRow(
		const vector<uint8_t>& input, size_t begin, size_t end) {
	ResidualRowKey row;
	row.active = true;
	size_t position = begin;
	const uint64_t terms = readCacheNumber(input, position, end);
	for (uint64_t i = 0; i < terms; ++i) {
		skipCacheBig(input, position, end);
		skipCacheBig(input, position, end);
	}
	const size_t gap_begin = position;
	skipCacheBig(input, position, end);
	if (position != end) throw logic_error("trailing residual row data");
	row.row_template.bytes.assign(input.begin() + begin, input.begin() + gap_begin);
	row.row_template.hash = cacheHash(row.row_template.bytes);
	row.gap.assign(input.begin() + gap_begin, input.begin() + end);
	return row;
}

RowTemplatePool::RowTemplatePool() {
	entries_.emplace_back();
}

RowTemplateID RowTemplatePool::find(const ReferenceKey& key) const {
	auto bucket = index_.find(key.hash);
	if (bucket == index_.end()) return 0;
	for (RowTemplateID id : bucket->second) {
		if (id < entries_.size() && entries_[id].alive && entries_[id].key == key)
			return id;
	}
	return 0;
}

RowTemplateID RowTemplatePool::probe(const ReferenceKey& key) {
	requests_++;
	return find(key);
}

RowTemplateID RowTemplatePool::acquire(const ReferenceKey& key) {
	requests_++;
	RowTemplateID id = find(key);
	if (id != 0) {
		Entry& entry = entries_[id];
		reuses_++;
		if (entry.references == 0) {
			wakes_++;
			entry.queued = false;
		}
		entry.references++;
		return id;
	}

	id = entries_.size();
	entries_.emplace_back();
	Entry& entry = entries_.back();
	entry.key = key;
	entry.references = 1;
	entry.alive = true;
	index_[key.hash].push_back(id);
	return id;
}

void RowTemplatePool::release(RowTemplateID id) {
	if (id == 0 || id >= entries_.size() || !entries_[id].alive ||
			entries_[id].references == 0)
		throw logic_error("invalid row-template release");
	Entry& entry = entries_[id];
	entry.references--;
	if (entry.references == 0 && !entry.queued) {
		entry.queued = true;
		unused_.push_back(id);
	}
}

void RowTemplatePool::reclaim(bool all) {
	if (!all && unused_.size() < 256) return;
	const size_t count = all ? unused_.size() : unused_.size() / 2;
	for (size_t i = 0; i < count; ++i) {
		const RowTemplateID id = unused_.front();
		unused_.pop_front();
		if (id == 0 || id >= entries_.size()) continue;
		Entry& entry = entries_[id];
		if (!entry.alive || !entry.queued || entry.references != 0) continue;
		auto bucket = index_.find(entry.key.hash);
		if (bucket != index_.end()) {
			auto& ids = bucket->second;
			ids.erase(remove(ids.begin(), ids.end(), id), ids.end());
			if (ids.empty()) index_.erase(bucket);
		}
		vector<uint8_t>().swap(entry.key.bytes);
		entry.key.hash = 0;
		entry.alive = false;
		entry.queued = false;
		reclaimed_++;
	}
}

uint64_t RowTemplatePool::sizeInBytes() const {
	uint64_t bytes = sizeof(RowTemplatePool) + entries_.capacity() * sizeof(Entry);
	for (const Entry& entry : entries_) bytes += entry.key.bytes.capacity();
	for (const auto& bucket : index_)
		bytes += sizeof(bucket) + bucket.second.capacity() * sizeof(RowTemplateID);
	bytes += unused_.size() * sizeof(RowTemplateID);
	return bytes;
}

template <typename T_data>
unsigned PackedComponent<T_data>::_bits_per_clause = 0;
template <typename T_data>
unsigned PackedComponent<T_data>::_bits_per_variable = 0;
template <typename T_data>
unsigned PackedComponent<T_data>::_variable_mask = 0;
template <typename T_data>
unsigned PackedComponent<T_data>::_clause_mask = 0;

#if 0
int availableRAMSize(){
#ifdef USE_SYSINFO
	struct sysinfo info;
	sysinfo(&info);

	uint64_t free_ram_bytes = info.freeram *(uint64_t) info.mem_unit;
	int free_ram = free_ram_bytes / 1048576;

	int maximum_cache_size = (int) opt_cachesize;

	if (opt_cachesize <= 0 || free_ram <= 0) {
		printf("c o Not enough memory to run.\n");
		exit(1);
	}

	if (opt_cachesize > free_ram) {
		maximum_cache_size = 7 * free_ram / 10;
#ifdef DEBUG
		printf("c o WARNING: Maximum cache size larger than free RAM available\n");
		printf("c o Free RAM %d MB\n", free_ram);
		printf("c o Maximum cache size : %d MB -> %d MB\n", (int) opt_cachesize, maximum_cache_size);
#endif
	}
	return maximum_cache_size;

#else
	return default_cachesize;
#endif
}
#endif

template <class T_data>
void ComponentCache<T_data>::init(int cachesize, CacheMode mode) {
	mode_ = mode;
	templates_ = RowTemplatePool();
	table_.clear();
	entry_base_.clear();
	entry_base_.reserve(2000000);
	entry_base_.push_back(new CachedComponent<T_data>()); // dummy Element
	table_.resize(bypass() ? 1 : (residual() ? 65537 : 900001), NULL);
	free_entry_base_slots_.clear();
	free_entry_base_slots_.reserve(10000);

	// maximum_cache_size_bytes = (int64_t) availableRAMSize() * 1048576;
	maximum_cache_size_bytes = (int64_t) cachesize * 1048576;
	my_time_ = 0;
	num_occupied_buckets_ = 0;
	num_cache_hits_ = 0;
	num_cache_look_ups_ = 0;
	num_cache_inserts_ = 0;
	num_cache_reduce_ = 0;
	sum_cache_hit_sizes_ = 0;
	num_cached_components_ = 0;
	sum_size_cached_components_ = 0;

	recompute_bytes_memory_usage();
}

template <class T_data>
ComponentCache<T_data>::~ComponentCache() {
	for (CachedComponent<T_data>* entry : entry_base_) {
		if (entry == nullptr) continue;
		releaseTemplates(*entry);
		delete entry;
	}
	templates_.reclaim(true);
	for (CacheBucket<T_data>* bucket : table_) delete bucket;
}

template <class T_data>
PendingComponentKey ComponentCache<T_data>::pendingKey(
		const ReferenceKey& key) const {
	PendingComponentKey pending;
	const vector<uint8_t>& input = key.bytes;
	size_t position = 0;
	const uint64_t version = readCacheNumber(input, position, input.size());
	if (version != 2) throw logic_error("unsupported residual cache key version");
	const uint64_t variables = readCacheNumber(input, position, input.size());
	for (uint64_t i = 0; i < variables; ++i)
		readCacheNumber(input, position, input.size());
	pending.variables.assign(input.begin(), input.begin() + position);

	const uint64_t records = readCacheNumber(input, position, input.size());
	pending.records.reserve(records);
	for (uint64_t i = 0; i < records; ++i) {
		const uint64_t length = readCacheNumber(input, position, input.size());
		if (length > input.size() - position)
			throw logic_error("truncated residual cache record");
		const size_t end = position + static_cast<size_t>(length);
		if (position >= end) throw logic_error("empty residual cache record");
		ResidualRecordKey record;
		record.kind = input[position++];
		if (record.kind == 1) {
			record.lower = splitResidualRow(input, position, end);
			position = end;
		} else if (record.kind == 2) {
			if (position >= end) throw logic_error("missing equality lower state");
			if (input[position++]) {
				const uint64_t row_length = readCacheNumber(input, position, end);
				if (row_length > end - position)
					throw logic_error("truncated equality lower row");
				record.lower = splitResidualRow(
						input, position, position + static_cast<size_t>(row_length));
				position += static_cast<size_t>(row_length);
			}
			if (position >= end) throw logic_error("missing equality upper state");
			if (input[position++]) {
				const uint64_t row_length = readCacheNumber(input, position, end);
				if (row_length > end - position)
					throw logic_error("truncated equality upper row");
				record.upper = splitResidualRow(
						input, position, position + static_cast<size_t>(row_length));
				position += static_cast<size_t>(row_length);
			}
			if (position != end) throw logic_error("trailing equality cache data");
		} else {
			throw logic_error("invalid residual cache record kind");
		}
		pending.records.emplace_back(std::move(record));
	}
	if (position != input.size()) throw logic_error("trailing residual cache key data");
	return pending;
}

template <class T_data>
bool ComponentCache<T_data>::compactKey(
		const PendingComponentKey& pending, bool take_references,
		CompactComponentKey& compact, vector<RowTemplateID>& references) {
	vector<vector<uint8_t>> records;
	records.reserve(pending.records.size());
	for (const ResidualRecordKey& source : pending.records) {
		vector<uint8_t> record;
		record.push_back(source.kind);
		auto appendRow = [&](const ResidualRowKey& row) {
			record.push_back(row.active ? 1 : 0);
			if (!row.active) return true;
			const RowTemplateID id = take_references
					? templates_.acquire(row.row_template)
					: templates_.probe(row.row_template);
			if (id == 0) return false;
			if (take_references) references.push_back(id);
			appendCacheNumber(id, record);
			appendCacheNumber(row.gap.size(), record);
			record.insert(record.end(), row.gap.begin(), row.gap.end());
			return true;
		};
		if (source.kind == 1) {
			if (!appendRow(source.lower)) return false;
		} else {
			if (!appendRow(source.lower) || !appendRow(source.upper)) return false;
		}
		records.emplace_back(std::move(record));
	}
	sort(records.begin(), records.end());
	compact.bytes = pending.variables;
	appendCacheNumber(records.size(), compact.bytes);
	for (const vector<uint8_t>& record : records) {
		appendCacheNumber(record.size(), compact.bytes);
		compact.bytes.insert(compact.bytes.end(), record.begin(), record.end());
	}
	compact.hash = cacheHash(compact.bytes);
	return true;
}

template <class T_data>
void ComponentCache<T_data>::releaseTemplates(CachedComponent<T_data>& component) {
	for (RowTemplateID id : component.templateRefs()) templates_.release(id);
	component.templateRefs().clear();
}

template <class T_data>
void ComponentCache<T_data>::eraseEntry(CacheEntryID id) {
	CachedComponent<T_data>* component = entry_base_[id];
	cache_bytes_memory_usage_ -= component->SizeInBytes();
	if (legacy()) sum_size_cached_components_ -= component->num_variables();
	releaseTemplates(*component);
	num_cached_components_--;
	delete component;
	entry_base_[id] = nullptr;
	free_entry_base_slots_.push_back(id);
	templates_.reclaim(false);
}

template <class T_data>
CacheEntryID ComponentCache<T_data>::createEntryFor(Component &comp, unsigned stack_id, ComponentManager<T_data> & manager) {
	my_time_++;
	CacheEntryID id;

	if (!bypass() && cache_bytes_memory_usage_ >= maximum_cache_size_bytes) {
		if (deleteEntries()) num_cache_reduce_++;
	}

	if (free_entry_base_slots_.empty()) {
		if (entry_base_.capacity() == entry_base_.size()) {
			entry_base_.reserve(2 * entry_base_.size());
		}
		entry_base_.push_back(legacy()
				? new CachedComponent<T_data>(comp, manager)
				: new CachedComponent<T_data>());
		id = entry_base_.size() - 1;
	} else {
		id = free_entry_base_slots_.back();
		assert(id < entry_base_.size());
		assert(entry_base_[id] == nullptr);
		free_entry_base_slots_.pop_back();
		entry_base_[id] = legacy()
				? new CachedComponent<T_data>(comp, manager)
				: new CachedComponent<T_data>();
	}
	entry_base_[id]->setComponentStackID(stack_id);
	entry_base_[id]->set_creation_time(my_time_);

	assert(entry_base_[id]->first_descendant() == 0);
	assert(entry_base_[id]->next_sibling() == 0);
	cache_bytes_memory_usage_ += entry_base_[id]->SizeInBytes();
	if (legacy())
		sum_size_cached_components_ += entry_base_[id]->num_variables();
	num_cached_components_++;

	return id;
}

#if 0
void ComponentCache::test_descendantstree_consistency() {
	for (unsigned id = 2; id < entry_base_.size(); id++)
		if (entry_base_[id] != nullptr) {
			CacheEntryID act_child = entry(id).first_descendant();
			while (act_child) {
				CacheEntryID next_child = entry(act_child).next_sibling();
				assert(entry(act_child).father() == id);

				act_child = next_child;
			}
			CacheEntryID father = entry(id).father();
			CacheEntryID act_sib = entry(father).first_descendant();
#ifndef NDEBUG
			bool found = false;
#endif
			while (act_sib) {
				CacheEntryID next_sib = entry(act_sib).next_sibling();
#ifndef NDEBUG
				if (act_sib == id)
					found = true;
#endif
				act_sib = next_sib;
			}
			assert(found);
		}
}
#endif

template <class T_data>
uint64_t ComponentCache<T_data>::recompute_bytes_memory_usage() {
	cache_bytes_memory_usage_ = sizeof(ComponentCache) + sizeof(CacheBucket<T_data> *) * table_.capacity();
	cache_bytes_memory_usage_ += templates_.sizeInBytes();
	for (auto pbucket : table_)
		if (pbucket != nullptr)
			cache_bytes_memory_usage_ += pbucket->getBytesMemoryUsage();
	for (auto pentry : entry_base_)
		if (pentry != nullptr)
			cache_bytes_memory_usage_ += pentry->SizeInBytes();

	return cache_bytes_memory_usage_;
}

template <class T_data>
CacheEntryID ComponentCache<T_data>::createEntryFor(Component &comp, unsigned stack_id, ComponentManager<T_data> & manager, const vec<lbool>& assigns) {
	my_time_++;
	CacheEntryID id;

	if (!bypass() && cache_bytes_memory_usage_ >= maximum_cache_size_bytes) {
		if (deleteEntries()) num_cache_reduce_++;
	}

	if (free_entry_base_slots_.empty()) {
		if (entry_base_.capacity() == entry_base_.size()) {
			entry_base_.reserve(2 * entry_base_.size());
		}
		entry_base_.push_back(legacy()
				? new CachedComponent<T_data>(comp, manager, assigns)
				: new CachedComponent<T_data>());
		id = entry_base_.size() - 1;
	} else {
		id = free_entry_base_slots_.back();
		assert(id < entry_base_.size());
		assert(entry_base_[id] == nullptr);
		free_entry_base_slots_.pop_back();
		entry_base_[id] = legacy()
				? new CachedComponent<T_data>(comp, manager, assigns)
				: new CachedComponent<T_data>();
	}
	if (residual())
		entry_base_[id]->setPendingKey(pendingKey(manager.referenceKey(comp, assigns)));
	entry_base_[id]->setComponentStackID(stack_id);
	entry_base_[id]->set_creation_time(my_time_);

	assert(entry_base_[id]->first_descendant() == 0);
	assert(entry_base_[id]->next_sibling() == 0);
	cache_bytes_memory_usage_ += entry_base_[id]->SizeInBytes();
	if (legacy())
		sum_size_cached_components_ += entry_base_[id]->num_variables();
	num_cached_components_++;

	return id;
}

template <class T_data>
bool ComponentCache<T_data>::requestValueOf(Component &comp, T_data &rn, NodeIndex &node) {
	if (bypass()) return false;
	CachedComponent<T_data> &packedcomp = entry(comp.id());
	num_cache_look_ups_++;

	if (residual()) {
		CompactComponentKey key;
		vector<RowTemplateID> references;
		if (!compactKey(packedcomp.pendingKey(), false, key, references)) return false;
		const unsigned int bucket_id = clip(key.hash);
		if (!isBucketAt(bucket_id)) return false;
		for (CacheEntryID id : *table_[bucket_id]) {
			CachedComponent<T_data>& candidate = entry(id);
			if (candidate.compactKey() == key) {
				num_cache_hits_++;
				sum_cache_hit_sizes_ += comp.nVars();
				rn = candidate.model_count();
				node = candidate.get_DTNode();
				return true;
			}
		}
		return false;
	}

	unsigned int v = clip(packedcomp.hashkey());
	if (!isBucketAt(v))
		return false;

	CachedComponent<T_data> *pcomp;
	for (auto it = table_[v]->begin(); it != table_[v]->end(); it++) {
		pcomp = &entry(*it);
		if (packedcomp.hashkey() == pcomp->hashkey()
				&& pcomp->equals(packedcomp)) {
			num_cache_hits_++;

			sum_cache_hit_sizes_ += pcomp->num_variables();
			rn = pcomp->model_count();
			node = pcomp->get_DTNode();
			// pcomp->set_creation_time(my_time_);
			return true;
		}
	}
	return false;
}

template <class T_data>
bool ComponentCache<T_data>::deleteEntries() {
	assert(cache_bytes_memory_usage_ >= maximum_cache_size_bytes);

	vector<double> scores;
	for (auto it = entry_base_.begin() + 1; it != entry_base_.end(); it++)
		if (*it != nullptr && (*it)->deletable()) {
			scores.push_back((double) (*it)->creation_time());
		}
	if (scores.empty()) return false;
	sort(scores.begin(), scores.end());
	double cutoff = scores[scores.size() / 2];

	//cout << "cutoff" << cutoff  << " entries: "<< entry_base_.size()<< endl;

	// first : go through the EntryBase and mark the entries to be deleted as deleted (i.e. EMPTY
	// note we start at index 2,
	// since index 1 is the whole formula,
	// should always stay here!
	for (unsigned id = 2; id < entry_base_.size(); id++)
		if (entry_base_[id] != nullptr && entry_base_[id]->deletable()) {
			double as = (double) entry_base_[id]->creation_time();
			if (as <= cutoff) {
				removeFromDescendantsTree(id);
				eraseEntry(id);
			}
		}
	// then go through the Hash Table and erase all Links to empty entries
	for (auto pbucket : table_)
		if (pbucket != nullptr) {
			for (auto bt = pbucket->rbegin(); bt != pbucket->rend(); bt++) {
				if (entry_base_[*bt] == nullptr) {
					*bt = pbucket->back();
					pbucket->pop_back();
				}
			}
		}
#if 0
	test_descendantstree_consistency();
#endif

	sum_size_cached_components_ = 0;
	num_cached_components_ = 0;
	for (unsigned id = 2; id < entry_base_.size(); id++)
		if (entry_base_[id] != nullptr) {
			if (legacy()) sum_size_cached_components_ += entry_base_[id]->num_variables();
			num_cached_components_++;
		}

	templates_.reclaim(false);
	cache_bytes_memory_usage_ = recompute_bytes_memory_usage();
#ifdef DEBUG
	cout << "c o Cache reduced to size " << recompute_bytes_memory_usage() / 1000000 << "MB" << endl;
	//cout << " \t entries: "<< entry_base_.size() - free_entry_base_slots_.size()<< endl;
#endif
	return true;
}

template <class T_data>
void ComponentCache<T_data>::deleteallentries(){
	for (unsigned id = 2; id < entry_base_.size(); id++)
		if (entry_base_[id] != nullptr) {
			removeFromDescendantsTree(id);
			eraseEntry(id);
		}

	for (auto pbucket : table_)
		if (pbucket != nullptr) {
			for (auto bt = pbucket->rbegin(); bt != pbucket->rend(); bt++) {
				if (entry_base_[*bt] == nullptr) {
					*bt = pbucket->back();
					pbucket->pop_back();
				}
			}
		}

	sum_size_cached_components_ = 0;
	num_cached_components_ = entry_base_.size() > 1 && entry_base_[1] != nullptr ? 1 : 0;
	templates_.reclaim(false);
	cache_bytes_memory_usage_ = recompute_bytes_memory_usage();
}

template <class T_data>
void PackedComponent<T_data>::adjustPackSize(unsigned int maxVarId, unsigned int maxClId) {
	_bits_per_variable = (unsigned int) ceil(log((double) maxVarId + 1) / log(2.0));
	_bits_per_clause = (unsigned int) ceil(log((double) maxClId + 1) / log(2.0));

	_variable_mask = _clause_mask = 0;
	for (unsigned int i = 0; i < _bits_per_variable; i++)
		_variable_mask = (_variable_mask << 1) + 1;
	for (unsigned int i = 0; i < _bits_per_clause; i++)
		_clause_mask = (_clause_mask << 1) + 1;
}

template class GPMC::PackedComponent<mpz_class>;
template class GPMC::ComponentCache<mpz_class>;
template class GPMC::PackedComponent<mpfr::mpreal>;
template class GPMC::ComponentCache<mpfr::mpreal>;
