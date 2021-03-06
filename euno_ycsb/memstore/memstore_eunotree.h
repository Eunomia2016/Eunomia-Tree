/*
Author: wangxinalex
Email: wangxinalex@gmail.com
Date: 2016/03/20
*/
#ifndef MEMSTOREEUNOTREE_H
#define MEMSTOREEUNOTREE_H

#include <stdlib.h>
#include <iostream>
#include <unordered_map>
#include <algorithm>
#include <utmpx.h>
#include "util/rtmScope.h"
#include "util/rtm.h"
#include "util/rtm_arena.h"
#include "util/mutexlock.h"
#include "util/numa_util.h"
#include "util/statistics.h"
#include "util/bloomfilter.h"
#include "util/ccm.h"
#include "port/port_posix.h"
#include "memstore.h"
#include "silo_benchmark/util.h"
//#define LEAF_NUM 15
#define N  15

//#define SEG_NUM 2
//#define SEG_LEN 7
//#define LEAF_NUM (SEG_NUM*SEG_LEN)

#define BTREE_PROF 0
#define BTREE_LOCK 0
#define BTPREFETCH 0
#define DUMMY 1

#define NODEMAP  0
#define NODEDUMP 0
#define KEYDUMP  0
#define KEYMAP   0
#define NUMADUMP 0

#define REMOTEACCESS 0
#define LEVEL_LOG 0

#define BUFFER_TEST 0
#define BUFFER_LEN (1<<8)
#define HASH_MASK (BUFFER_LEN-1)
#define OFFSET_BITS 4

#define BM_TEST 0
#define FLUSH_FREQUENCY 100

#define SHUFFLE_KEYS 0

#define ORIGIN_INSERT 0
#define SHUFFLE_INSERT 1
#define UNSORTED_INSERT 0

#define SEGS     4
#define EMP_LEN  4
#define HAL_LEN  (EMP_LEN/2)
#define LEAF_NUM (SEGS*EMP_LEN)

#define CONTENTION_LEVEL 5

#define ADAPTIVE_LOCK 1
#define DEFAULT_LOCK 0
#define BM_QUERY 1
#define BM_PROF 0

#define SPEC_PROF 0

#define DUP_PROF 0

#define SPLIT_DEPTH 3
#define NO_SEQ_NO 0

#define ERROR_RATE 0.05
#define BM_SIZE 16

#define TIME_BKD 0
#define THRESHOLD 2

using namespace std;
namespace leveldb {

struct access_log {
	uint64_t gets;
	uint64_t writes;
	uint64_t splits;
};

struct key_log {
	uint64_t gets;
	uint64_t writes;
	uint64_t dels;
};

static uint32_t leaf_id = 0;
static int32_t inner_id = 0;
static uint32_t table_id = 0;
#define CACHE_LINE_SIZE 64

class MemstoreEunoTree: public Memstore {
//Test purpose
public:
	inline void set_thread_num(unsigned thread) {
		thread_num = thread;
	}
	inline void set_b() {
		goto_b ^ 1;
	}
	int tableid;
	uint64_t insert_seq;

	uint64_t rconflict = 0;
	uint64_t wconflict = 0;

	uint64_t spec_hit;
	uint64_t spec_miss;

	uint64_t shifts;

	uint64_t half_born;
	uint64_t empty_born;

	uint64_t should_protect;
	uint64_t should_not_protect;

	uint64_t leaf_rightmost;
	uint64_t leaf_not_rightmost;

	bool first_leaf;

	uint64_t insert_time;

	uint64_t insert_times[SEGS + 1];
	uint64_t inserts[SEGS + 1];

	uint64_t duplicate_keys;
	uint64_t dup_found, leaf_inserts, leaf_splits;
	uint64_t original_inserts, scope_inserts;

	uint64_t stage_1_time, bm_time, stage_2_time;

	uint64_t inner_rightmost, inner_not_rightmost;

	uint64_t bm_found_num, bm_miss_num;

	uint64_t consist, inconsist;
	uint64_t node_difference;
	uint64_t node_inserts;
	bool contention_mode;
	
	SpinLock bm_lock;
	struct KeyValue {
		uint64_t key;
		MemNode* value;
	};

	struct {
		bool operator()(const struct KeyValue& a, const struct KeyValue& b) {
			return a.key < b.key;
		}
	} KVCompare;


#if SHUFFLE_INSERT
	struct Leaf_Seg {
		//key-value stored in the leaf segment
		KeyValue kvs[EMP_LEN];
		unsigned max_room;
		unsigned key_num;
		//cacheline alignment
		//remove
		uint64_t paddings1[7]; 
		bool isFull() {
			return key_num >= max_room;
		}
	};

	struct Insert_Log {
		Insert_Log(): one_try(0), two_try(0), check_all(0), split(0) {}
		uint64_t one_try;
		uint64_t two_try;
		uint64_t check_all;
		uint64_t split;
	};

	Insert_Log insert_log;
#endif
	struct InnerNode; //forward declaration

	
	struct LeafNode {
		LeafNode() : num_keys(0), bm_filter(NULL), seq(0), reserved(NULL), kvs_num(0), ccm(new CCM()) {
			
		}
		
#if SHUFFLE_INSERT
		//leaf segments in a leaf node
		Leaf_Seg leaf_segs[SEGS];
		//remove
		uint64_t paddings[8];//cacheline alignment
		//read and write lock
		SpinLock mlock;
#endif
		//reserved keys
		KeyValue kvs[LEAF_NUM];//16*16
		unsigned kvs_num;
		BloomFilter* bm_filter;
		KeyValue* reserved;
		CCM* ccm;

		//--896 bytes--
		unsigned num_keys;
		LeafNode *left;
		LeafNode *right;
		InnerNode* parent;
		uint64_t seq;
	public:		
		inline int total_keys() {
			int total_keys = 0;
			for(int i = 0 ; i < SEGS; i++) {
				total_keys += leaf_segs[i].key_num;
			}
			return total_keys;
		}
		void moveToReserved() {
			//move the keys to reserved room
			int temp_idx = kvs_num;
		
			for(int i = 0; i < SEGS; i++) {
				for(int j = 0; j < leaf_segs[i].key_num; j++) {
					kvs[temp_idx] = leaf_segs[i].kvs[j];
					temp_idx++;
					kvs_num++;
				}
				leaf_segs[i].key_num = 0;
			}
		}
		MemNode* insertSegment(int idx, uint64_t key) {
			leaf_segs[idx].kvs[leaf_segs[idx].key_num].key = key;
			MemNode* reMem = NULL;

#if DUMMY
			leaf_segs[idx].kvs[leaf_segs[idx].key_num].value = dummyval_;
			reMem = dummyval_;
#else
			leaf_segs[idx].kvs[leaf_segs[idx].key_num].value = GetMemNode();
			reMem = leaf_segs[idx].kvs[leaf_segs[idx].key_num].value ;
#endif
			assert(reMem != NULL);

			leaf_segs[idx].key_num++;
			dummyval_ = NULL;
			return reMem;	
		}
		inline bool hasRoom() {
			unsigned total_seg = total_keys();
			unsigned total = total_seg + kvs_num;
			unsigned upper = (leaf_segs[0].max_room * SEGS) >> 1;
			return (total < LEAF_NUM) && (total_seg <= upper);		
		}
		void shrinkSegs() {
			if(leaf_segs[0].max_room > 2) {
				for(int i = 0; i < SEGS; i++) {
					leaf_segs[i].max_room = leaf_segs[i].max_room >> 1;
				}
			}
		}
		inline bool isFull() {
			return (total_keys() + kvs_num) >= LEAF_NUM;
		}
	};

	struct InnerNode {
		InnerNode() : num_keys(0) {
			
		}
		uint64_t 	 keys[N];
		void*	 children[N + 1];

		unsigned num_keys;
		InnerNode* parent;
	};

	//The result object of the delete function
	struct DeleteResult {
		DeleteResult(): value(0), freeNode(false), upKey(-1) {}
		Memstore::MemNode* value;  //The value of the record deleted
		bool freeNode;	//if the children node need to be free
		uint64_t upKey; //the key need to be updated -1: default value
	};

	class Iterator: public Memstore::Iterator {
	public:
		// Initialize an iterator over the specified list.
		// The returned iterator is not valid.
		Iterator() {};
		Iterator(MemstoreEunoTree* tree);

		// Returns true iff the iterator is positioned at a valid node.
		bool Valid();

		// Returns the key at the current position.
		// REQUIRES: Valid()
		MemNode* CurNode();

		uint64_t Key();

		// Advances to the next position.
		// REQUIRES: Valid()
		bool Next();

		// Advances to the previous position.
		// REQUIRES: Valid()
		bool Prev();

		// Advance to the first entry with a key >= target
		void Seek(uint64_t key);

		void SeekPrev(uint64_t key);

		// Position at the first entry in list.
		// Final state of iterator is Valid() iff list is not empty.
		void SeekToFirst();

		// Position at the last entry in list.
		// Final state of iterator is Valid() iff list is not empty.
		void SeekToLast();

		uint64_t* GetLink();

		uint64_t GetLinkTarget();

	private:
		MemstoreEunoTree* tree_;
		LeafNode* node_;
		uint64_t seq_;
		int leaf_index;
		uint64_t *link_;
		uint64_t target_;
		uint64_t key_;
		MemNode* value_;
		uint64_t snapshot_;

		
		// Intentionally copyable
	};

public:
	
	MemstoreEunoTree() {
		insert_seq = 0;
		
		depth = 0;
#if BTREE_PROF
		writes = 0;
		reads = 0;
		calls = 0;
#endif
	}
	MemstoreEunoTree(int _tableid) {
		insert_seq = 0;
		first_leaf = true;
		depth = 0;

		tableid = _tableid;
		insert_time = 0;
		spec_hit = spec_miss = 0;
		duplicate_keys = 0;
		dup_found = leaf_splits = leaf_inserts = 0;
		original_inserts = scope_inserts = 0;
		stage_1_time = stage_2_time = bm_time = 0;
		consist = inconsist = 0;
		node_difference = 0;
		node_inserts = 0;
		contention_mode = true;
		
		leaf_rightmost = leaf_not_rightmost = 0;
#if BTREE_PROF
		writes = 0;
		reads = 0;
		calls = 0;
#endif
	}

	~MemstoreEunoTree() {
		
#if SPEC_PROF
		printf("spec_hit = %lu, spec_miss = %lu\n", spec_hit, spec_miss);
#endif

#if BTREE_PROF
		printf("calls %ld avg %f writes %f\n", calls, (float)(reads + writes) / (float)calls, (float)(writes) / (float)calls);
#endif
	}

	void transfer_para(RTMPara& para) {
		prof.transfer_para(para);
	}

	inline void ThreadLocalInit() {
		if(false == localinit_) {
			arena_ = new RTMArena();
			dummyval_ = GetMemNode();
			dummyval_->value = NULL;
			dummyleaf_ = new LeafNode();
			localinit_ = true;
		}
	}

	inline LeafNode* new_leaf_node() {
#if DUMMY
		LeafNode* result = dummyleaf_;
		dummyleaf_ = NULL;
#else
		LeafNode* result = new LeafNode();
#endif
		return result;
	}

	inline InnerNode* new_inner_node() {
		InnerNode* result = new InnerNode();
		return result;
	}

	inline LeafNode* FindLeaf(uint64_t key) {
		InnerNode* inner;
		register void* node = root;
		register unsigned d = depth;
		unsigned index = 0;
		while(d-- != 0) {
			index = 0;
			inner = reinterpret_cast<InnerNode*>(node);
			while((index < inner->num_keys) && (key >= inner->keys[index])) {
				++index;
			}
			node = inner->children[index];

		}
		return reinterpret_cast<LeafNode*>(node);
	}
	//NOUSE
	inline InnerNode* SpecInner(uint64_t key) {
		InnerNode* inner;
		register void* node = root;
		register unsigned d = depth;
		unsigned index = 0;
		if(d == 0) {
			return reinterpret_cast<InnerNode*>(node);
		}
		while(d-- != 1) {
			index = 0;
			inner = reinterpret_cast<InnerNode*>(node);
			while((index < inner->num_keys) && (key >= inner->keys[index])) {
				++index;
			}
			node = inner->children[index];
		}
		return reinterpret_cast<InnerNode*>(node);
	}
	//return a memNode if the key is found in one of leaf node
	inline MemNode* FindKeyInLeaf(LeafNode* targetLeaf, uint64_t key) {
		for(int i = 0; i < targetLeaf->kvs_num; i++) {
			if(targetLeaf->kvs[i].key == key) {
				return targetLeaf->kvs[i].value;;
			}
		}
		for(int i = 0; i < SEGS; i++) {
			for(int j = 0; j < targetLeaf->leaf_segs[i].key_num; j++) {
				if(targetLeaf->leaf_segs[i].kvs[j].key == key) {
					return targetLeaf->leaf_segs[i].kvs[j].value;
				}
			}
		}
		return NULL;
	}

	inline MemNode* Get(uint64_t key) {
		if(root==NULL) return NULL;
		LeafNode* targetLeaf = NULL;
		
TOP_RETRY:
		uint64_t seqno = 0;
		unsigned int retry = 0;
		{
			RTMScope begtx(&prof, depth * 2, 1, &rtmlock, GET_TYPE);
			ScopeFind(key, &targetLeaf);
			seqno = targetLeaf->seq;
		}
		unsigned int slot = targetLeaf->ccm->getIndex(key);
		
#if BM_QUERY
		//check if the key is in the leaf node
		if(!targetLeaf->ccm->skipBF()) {
			targetLeaf->mlock.Lock();
			bool bm_found = queryBMFilter(targetLeaf, key, slot, 1);
			targetLeaf->mlock.Unlock();

			if(!bm_found) {
				return NULL;
			}
		}
#endif

		MemNode* res = NULL;
		bool consistent = true;
		{
			RTMScope begtx(&prof, depth * 2, 1, &rtmlock, GET_TYPE);
			if(targetLeaf->seq == seqno) {
				res = FindKeyInLeaf(targetLeaf, key);
			} else {
				consistent = false;
			}
			retry = begtx.getRetry();
		}
		targetLeaf->ccm->add_conflict_num(retry);
		if(!consistent) {
			goto TOP_RETRY;
		}
		return res;
		
	}

	inline MemNode* Put(uint64_t k, uint64_t* val) {
		ThreadLocalInit();
		
		MemNode *node = GetWithInsert(k).node;
		if(node == NULL){
			printf("k = %lu return NULL\n", k);
		}
		node->value = val;
#if BTREE_PROF
		reads = 0;
		writes = 0;
		calls = 0;
#endif
		return node;
	}

	inline int slotAtLeaf(uint64_t key, LeafNode* cur) {
		int slot = 0;
		while((slot < cur->num_keys) && cur->kvs[slot].key != key) {
			slot++;
		}
		return slot;
	}

	inline Memstore::MemNode* removeLeafEntry(LeafNode* cur, int slot) {
		assert(slot < cur->num_keys);
		cur->seq = cur->seq + 1;
		Memstore::MemNode* value = cur->kvs[slot].value;
		cur->num_keys--; //num_keys subtracts one
		//The key deleted is the last one
		if(slot == cur->num_keys)
			return value;
		//Re-arrange the entries in the leaf
		for(int i = slot + 1; i <= cur->num_keys; i++) {
			cur->kvs[i - 1] = cur->kvs[i];
		}
		return value;
	}

	inline DeleteResult* LeafDelete(uint64_t key, LeafNode* cur) {
		//step 1. find the slot of the key
		int slot = slotAtLeaf(key, cur);
		//the record of the key doesn't exist, just return
		if(slot == cur->num_keys) {
			return NULL;
		}
		DeleteResult *res = new DeleteResult();

		//step 2. remove the entry of the key, and get the deleted value
		res->value = removeLeafEntry(cur, slot);

		//step 3. if node is empty, remove the node from the list
		if(cur->num_keys == 0) {
			if(cur->left != NULL)
				cur->left->right = cur->right;
			if(cur->right != NULL)
				cur->right->left = cur->left;

			//Parent is responsible for the node deletion
			res->freeNode = true;
			return res;
		}
		//The smallest key in the leaf node has been changed, update the parent key
		if(slot == 0) {
			res->upKey = cur->kvs[0].key;
		}
		return res;
	}

	inline int slotAtInner(uint64_t key, InnerNode* cur) {
		int slot = 0;
		while((slot < cur->num_keys) && (cur->keys[slot] <= key)) {
			slot++;
		}
		return slot;
	}

	inline void removeInnerEntry(InnerNode* cur, int slot, DeleteResult* res) {
		assert(slot <= cur->num_keys);
		//If there is only one available entry
		if(cur->num_keys == 0) {
			assert(slot == 0);
			res->freeNode = true;
			return;
		}
		//The key deleted is the last one
		if(slot == cur->num_keys) {
			cur->num_keys--;
			return;
		}
		//rearrange the children slot
		for(int i = slot + 1; i <= cur->num_keys; i++)
			cur->children[i - 1] = cur->children[i];
		//delete the first entry, upkey is needed
		if(slot == 0) {
			//record the first key as the upkey
			res->upKey = cur->keys[slot];
			//delete the first key
			for(int i = slot; i < cur->num_keys - 1; i++) {
				cur->keys[i] = cur->keys[i + 1];
			}
		} else {
			//delete the previous key
			for(int i = slot; i < cur->num_keys; i++) {
				cur->keys[i - 1] = cur->keys[i];
			}
		}
		cur->num_keys--;
	}

	inline DeleteResult* InnerDelete(uint64_t key, InnerNode* cur , int depth) {
		DeleteResult* res = NULL;
		//step 1. find the slot of the key
		int slot = slotAtInner(key, cur);

		//step 2. remove the record recursively
		//This is the last level of the inner nodes
		if(depth == 1) {
			res = LeafDelete(key, (LeafNode *)cur->children[slot]);
		} else {
			res = InnerDelete(key, (InnerNode *)cur->children[slot], (depth - 1));
		}
		//The record is not found
		if(res == NULL) {
			return res;
		}
		//step 3. Remove the entry if the TOTAL children nodes have been removed
		if(res->freeNode) {
			//FIXME: Should free the children node here
			//remove the node from the parent node
			res->freeNode = false;
			removeInnerEntry(cur, slot, res);
			return res;
		}
		//step 4. update the key if needed
		if(res->upKey != -1) {
			if(slot != 0) {
				cur->keys[slot - 1] = res->upKey; //the upkey should be updated
				res->upKey = -1;
			}
		}
		return res;
	}

	inline Memstore::MemNode* Delete_rtm(uint64_t key) {
#if BTREE_LOCK
		MutexSpinLock lock(&slock);
#else
		RTMScope begtx(&prof, depth * 2, 1, &rtmlock, DEL_TYPE);
#endif
		DeleteResult* res = NULL;
		if(depth == 0) {
			//Just delete the record from the root
			res = LeafDelete(key, (LeafNode*)root);
		} else {
			res = InnerDelete(key, (InnerNode*)root, depth);
		}

		if(res == NULL)
			return NULL;

		if(res->freeNode)
			root = NULL;

		return res->value;
	}

	inline Memstore::MemNode* GetWithDelete(uint64_t key) {
		ThreadLocalInit();
		MemNode* value = Delete_rtm(key);
#if DUMMY
		if(dummyval_ == NULL) {
			dummyval_ = GetMemNode();
		}
		if(dummyleaf_ == NULL) {
			dummyleaf_ = new LeafNode();
		}
#endif
		return value;
	}

#if ADAPTIVE_LOCK
	inline bool ShouldLockLeaf(LeafNode* leaf) {
		
		int full_segs = 0;
		for(int i = 0; i < SEGS; i++) {
			if(leaf->leaf_segs[i].key_num >= leaf->leaf_segs[i].max_room) {
				full_segs++;
			}
			if(full_segs == 1) break;
		}
		bool need_protect = (full_segs >= 1);
		return need_protect;
		
	}
	inline bool is_conflict(LeafNode* leaf) {
		return leaf->ccm->is_conflict();
	}
#endif
	
	inline bool queryBMFilter(LeafNode* leafNode, uint64_t this_key, unsigned int slot, bool get) {
		bool bm_found = false;
		if(leafNode->ccm->mark_bits == 0) {
			bm_found = true;
			for(int i = 0; i < leafNode->kvs_num; i++) {
				uint64_t key = leafNode->kvs[i].key;
				unsigned int slot_temp = leafNode->ccm->getIndex(key);
				leafNode->ccm->set_mark_bits(key, slot_temp);
			}
			for(int i = 0; i < SEGS; i++) {
				for(int j = 0; j < leafNode->leaf_segs[i].key_num; j++) {
					uint64_t key = leafNode->leaf_segs[i].kvs[j].key;
					unsigned int slot_temp = leafNode->ccm->getIndex(key);
					leafNode->ccm->set_mark_bits(key, slot_temp);
				}
			}
		}
		else {
			if(leafNode->ccm->isfound(this_key))
				bm_found = true;
		}
		if(!get) 
			leafNode->ccm->set_mark_bits(this_key, slot);
		return bm_found; 
	}

	inline Memstore::InsertResult GetWithInsert(uint64_t key) {
		bool should_sample = false;
		should_sample = key % 10 == 0;
		int cpu_id = sched_getcpu();
		ThreadLocalInit();
		LeafNode* target_leaf = NULL;

		MemNode* res = NULL;
		bool rtm = true;
		
		if(depth < CONTENTION_LEVEL || thread_num <= 2 || goto_b) {
TOP_B:		res = Insert_rtm(key, &target_leaf, should_sample);
		} else {
			LeafNode* leafNode = NULL;
			MemNode* memNode = NULL;
			int temp_depth;
			bool bm_found = true;
			bool locked = false;
			uint64_t this_key = key;
#if TIME_BKD
			util::timer t;
#endif
			uint64_t seqno = 0;
			int conflict_num = 0;
			int id = 5;
			int retry = 0;

TOP_RETRY:  {
				RTMScope begtx(&prof, depth * 2, 1, &rtmlock, GET_TYPE, should_sample, &conflict_num);
				temp_depth = ScopeFind(this_key, &leafNode);
				seqno = leafNode->seq;
				
			}
			

#if TIME_BKD
			stage_1_time += t.lap();
#endif
			
			unsigned int slot = leafNode->ccm->getIndex(this_key);
			
#if BM_QUERY
			if(!leafNode->ccm->skipBF()) {
				leafNode->mlock.Lock();
				bm_found = queryBMFilter(leafNode, this_key, slot, 0); //it should be atomic
				leafNode->mlock.Unlock();
			}
#endif

#if BM_PROF
			if(bm_found) {
				bm_found_num++;
			} 
			else {
				bm_miss_num++;
			}
#endif

#if TIME_BKD
			bm_time += t.lap();
#endif

#if DEFAULT_LOCK
			if(leafNode->total_keys() >= LEAF_NUM / 2) {
				leafNode->mlock.Lock();
			}
#endif

#if ADAPTIVE_LOCK
			if((bm_found || !ShouldLockLeaf(leafNode))) {
				locked = true;
				leafNode->mlock.Lock();
			}
			else {
				leafNode->ccm->read_Lock(slot);
			}
#endif
			bool consistent = true;
			{
				RTMScope begtx(&prof, temp_depth * 2, 1, &rtmlock, GET_TYPE, should_sample);
				if(leafNode->seq == seqno) {
					ScopeInsert(leafNode, this_key, &memNode, !bm_found, temp_depth);
				} else { //the node has been split -> re-search from the top
					consistent = false;
				}
				retry = begtx.getRetry();
				
			}
			leafNode->ccm->add_conflict_num(retry);
			
#if DEFAULT_LOCK
			leafNode->mlock.Unlock();
#endif

#if ADAPTIVE_LOCK
			if(locked) {
				leafNode->mlock.Unlock();
			}
			else {
				leafNode->ccm->read_Unlock(slot);	
			}
#endif
			if(!consistent) {
				goto TOP_RETRY;
			} 
#if TIME_BKD
			stage_2_time += t.lap();
#endif
			res = memNode;
			
		}

#if DUMMY
		if(dummyval_ == NULL) {
			dummyval_ = GetMemNode();
		}
		if(dummyleaf_ == NULL) {
			dummyleaf_ = new LeafNode();
		}
#endif
		
		return {res, false};
	}

	inline int ScopeFind(uint64_t key, LeafNode** leafNode) {
		LeafNode* leaf;
		InnerNode* inner;
		register void* node ;
		unsigned index ;
		register unsigned d;

		node = root;
		d = depth;

		while(d-- != 0) {
			index = 0;
			inner = reinterpret_cast<InnerNode*>(node);
			while((index < inner->num_keys) && (key >= inner->keys[index])) {
				++index;
			}
			node = inner->children[index];
		}
		leaf = reinterpret_cast<LeafNode*>(node);
		*leafNode = leaf;
		return depth;
	}

	inline LeafNode* ScopeSpec(InnerNode* inner, uint64_t key, int temp_depth) {
		register void* node;
		unsigned index;
		unsigned current_depth;
		InnerNode* temp_inner;

		node = inner;
		index = 0;
		current_depth = temp_depth - SPLIT_DEPTH;
		while(current_depth-- != 0) {
			index = 0;
			temp_inner = reinterpret_cast<InnerNode*>(node);
			while((index < temp_inner->num_keys) && (key >= temp_inner->keys[index])) {
				++index;
			}
			node = temp_inner->children[index];
		}

		LeafNode* leaf = reinterpret_cast<LeafNode*>(node);
		return leaf;
	}

	inline bool InnerContains(InnerNode* inner, uint64_t target_key) {
		for(int i = 0 ; i < inner->num_keys; i++) {
			if(inner->keys[i] == target_key) {
				return true;
			}
		}
		return false;
	}

	inline bool LeafContains(LeafNode* leaf, uint64_t target_key) {
		for(int i = 0; i < SEGS; i++) {
			for(int j = 0; j < leaf->leaf_segs[i].key_num; j++) {
				if(leaf->leaf_segs[i].kvs[j].key == target_key) {
					return true;
				}
			}
		}
		for(int i = 0; i < LEAF_NUM; i++) {
			if(leaf->kvs[i].key == target_key) {
				return true;
			}
		}

		return false;
	}

	inline void ScopeInsert(LeafNode* leaf, uint64_t key, MemNode** val, bool insert_only, int temp_depth) {
		unsigned current_depth;
		unsigned index;
		InnerNode* temp_inner;

		LeafNode* new_leaf = NULL;
		uint64_t leaf_upKey = 0;
		if(!insert_only) { //not only insert
			bool found = FindDuplicate(leaf, key, val);
			if(found) { //duplicate insertion
#if	DUP_PROF
				dup_found++;
#endif
				return ;
			}
		}

		//no need to find duplicate twice
		new_leaf = ShuffleLeafInsert(key, leaf, val, &leaf_upKey, true);//leaf_upKey is the key that should be inserted into the parent InnerNode

		if(new_leaf != NULL) { //should insert new leafnode
			InnerNode* insert_inner = new_leaf->parent;
			InnerNode* toInsert = insert_inner;
			int k = 0;
			while((k < insert_inner->num_keys) && (key >= insert_inner->keys[k])) {
				k++;
			}

			InnerNode* new_sibling = NULL;

			uint64_t inner_upKey = 0;
			//the inner node is full -> split it
			if(insert_inner->num_keys == N) {


				new_sibling = new_inner_node();
				new_sibling->parent = insert_inner->parent;
				if(new_leaf->leaf_segs[0].max_room == EMP_LEN) { //LeafNode is at rightmost
					new_sibling->num_keys = 0; //new sibling is also at rightmost
					inner_upKey = leaf_upKey;
					toInsert = new_sibling;
					k = -1;
				} else {
					unsigned threshold = (N + 1) / 2; //8
					//num_keys(new inner node) = num_keys(old inner node) - threshold
					new_sibling->num_keys = insert_inner->num_keys - threshold; //=>7
					//moving the excessive keys to the new inner node
					for(unsigned i = 0; i < new_sibling->num_keys; ++i) {
						new_sibling->keys[i] = insert_inner->keys[threshold + i];
						new_sibling->children[i] = insert_inner->children[threshold + i];
						reinterpret_cast<LeafNode*>(insert_inner->children[threshold + i])->parent = new_sibling;
					}

					//the LAST child, there is one more children than keys
					new_sibling->children[new_sibling->num_keys] = insert_inner->children[insert_inner->num_keys];
					reinterpret_cast<LeafNode*>(insert_inner->children[insert_inner->num_keys])->parent = new_sibling;
					//the num_key of the original node should be below the threshold
					insert_inner->num_keys = threshold - 1; //=>7
					//upkey should be the delimiter of the old/new node in their common parent
					uint64_t new_upKey = insert_inner->keys[threshold - 1]; //the largest key of the old innernode

					//the new leaf node could be the child of old/new inner node
					if(leaf_upKey >= new_upKey) {
						toInsert = new_sibling;//should insert at the new innernode
						//if the new inner node is to be inserted, the index to insert should subtract threshold
						if(k >= threshold) k = k - threshold;
						else k = 0; //or insert at the first slot
					}
					inner_upKey = new_upKey;
				}
//				inner->keys[N-1] = leaf_upKey;
				new_sibling->keys[N - 1] = inner_upKey; //???
			}

			//insert the new key at the (k)th slot of the parent node (old or new)
			if(k != -1) {
				for(int i = toInsert->num_keys; i > k; i--) { //move afterwards the remaining keys
					toInsert->keys[i] = toInsert->keys[i - 1];
					toInsert->children[i + 1] = toInsert->children[i];
				}
				toInsert->num_keys++; //add a new key
				toInsert->keys[k] = leaf_upKey;
			}

			toInsert->children[k + 1] = new_leaf;
			new_leaf->parent = toInsert;
			
			while(new_sibling != NULL) {

				uint64_t original_upKey = inner_upKey;
				InnerNode* child_sibling = new_sibling;
				InnerNode* insert_inner = new_sibling->parent;
				InnerNode* toInsert = insert_inner;

				if(toInsert == NULL) { //so the parent should be the new root
					InnerNode *new_root = new_inner_node();
					new_root->num_keys = 1;
					new_root->keys[0] = inner_upKey;
					new_root->children[0] = root;
					new_root->children[1] = new_sibling;

					reinterpret_cast<InnerNode*>(root)->parent = new_root;
					new_sibling->parent = new_root;

					root = new_root;
					reinterpret_cast<InnerNode*>(root)->parent = NULL;
					depth++;
					return;
				}

				new_sibling = NULL;
				k = 0;
				while((k < insert_inner->num_keys) && (key >= insert_inner->keys[k])) {
					k++;
				}
				

				unsigned threshold = (N + 1) / 2; //split equally =>8
				//the current node is full, creating a new node to hold the inserted key
				if(insert_inner->num_keys == N) {

					new_sibling = new_inner_node();

					new_sibling->parent = insert_inner->parent;

					if(child_sibling->num_keys == 0) {

						new_sibling->num_keys = 0;
						inner_upKey = child_sibling->keys[N - 1];
						toInsert = new_sibling;
						k = -1;
					} else {

						new_sibling->num_keys = insert_inner->num_keys - threshold;//=>7

						for(unsigned i = 0; i < new_sibling->num_keys; ++i) {
							new_sibling->keys[i] = insert_inner->keys[threshold + i];
							//printf("new_sibling->keys[%d] = %lu\n",i,new_sibling->keys[i]);
							new_sibling->children[i] = insert_inner->children[threshold + i];
							reinterpret_cast<InnerNode*>(insert_inner->children[threshold + i])->parent = new_sibling;
						}
						
						new_sibling->children[new_sibling->num_keys] = insert_inner->children[insert_inner->num_keys];
						reinterpret_cast<InnerNode*>(insert_inner->children[insert_inner->num_keys])->parent = new_sibling;

						insert_inner->num_keys = threshold - 1;//=>7
						
						inner_upKey = insert_inner->keys[threshold - 1]; //=>7
						//after split, the new key could be inserted into the old node or the new node
						if(key >= inner_upKey) {
							toInsert = new_sibling;
							if(k >= threshold) k = k - threshold;
							else k = 0;
						}
					}
					new_sibling->keys[N - 1] = inner_upKey;

				} else {
					new_sibling = NULL;
				}
				//inserting the new key to appropriate position
				if(k != -1) {
					for(int i = toInsert->num_keys; i > k; i--) {
						toInsert->keys[i] = toInsert->keys[i - 1];
						toInsert->children[i + 1] = toInsert->children[i];
					}
					toInsert->num_keys++;
					toInsert->keys[k] = child_sibling->keys[N - 1];

				}
				toInsert->children[k + 1] = child_sibling;
				child_sibling->parent = toInsert;
			}
		}
		
	}

	inline InnerNode* InnerInsert(uint64_t key, InnerNode *inner, int d, MemNode** val, LeafNode** target_leaf) {
		unsigned k = 0;
		uint64_t upKey;
		InnerNode *new_sibling = NULL;
		//find the appropriate position of the new key

		while((k < inner->num_keys) && (key >= inner->keys[k])) {
			k++;
		}

		void *child = inner->children[k]; //search the descendent layer

		//inserting at the lowest inner level
		if(d == 1) {
			uint64_t temp_upKey;
			LeafNode *new_leaf = ShuffleLeafInsert(key, reinterpret_cast<LeafNode*>(child), val, &temp_upKey, false);

			if(new_leaf != NULL) {	//if a new leaf node is created
				InnerNode *toInsert = inner;
				if(inner->num_keys == N) {

					new_sibling = new_inner_node();

					new_sibling->parent = inner->parent;

					if(new_leaf->leaf_segs[0].max_room == EMP_LEN) { //the new LeafNode is at rightmost

						new_sibling->num_keys = 0;
						upKey = temp_upKey;

						toInsert = new_sibling;
						k = -1;
					} else {

						unsigned threshold = (N + 1) / 2;
						new_sibling->num_keys = inner->num_keys - threshold;
						//moving the excessive keys to the new inner node
						for(unsigned i = 0; i < new_sibling->num_keys; ++i) {
							new_sibling->keys[i] = inner->keys[threshold + i];
							new_sibling->children[i] = inner->children[threshold + i];
							reinterpret_cast<LeafNode*>(inner->children[threshold + i])->parent = new_sibling;
						}

						//the last child
						new_sibling->children[new_sibling->num_keys] = inner->children[inner->num_keys];
						reinterpret_cast<LeafNode*>(inner->children[inner->num_keys])->parent = new_sibling;
						//the num_key of the original node should be below the threshold
						inner->num_keys = threshold - 1;
						//upkey should be the delimiter of the old/new node in their common parent

						upKey = inner->keys[threshold - 1];

						//the new leaf node could be the child of old/new inner node
						if(temp_upKey >= upKey) {
							toInsert = new_sibling;
							//if the new inner node is to be inserted, the index to insert should subtract threshold
							if(k >= threshold) k = k - threshold;
							else k = 0;
						}

					}
					new_sibling->keys[N - 1] = upKey; //???

				}

				//insert the new key at the (k)th slot of the parent node (old or new)
				if(k != -1) {
					for(int i = toInsert->num_keys; i > k; i--) {
						toInsert->keys[i] = toInsert->keys[i - 1];
						toInsert->children[i + 1] = toInsert->children[i];
					}
					toInsert->num_keys++; //add a new key
					toInsert->keys[k] = temp_upKey; //subtle bugs
				}

				toInsert->children[k + 1] = new_leaf;
				new_leaf->parent = toInsert;

			}
		} else { //not inserting at the lowest inner level
			//recursively insert at the lower levels
			InnerNode *new_inner =
				InnerInsert(key, reinterpret_cast<InnerNode*>(child), d - 1, val, target_leaf);

			if(new_inner != NULL) {

				InnerNode *toInsert = inner;
				InnerNode *child_sibling = new_inner;

				unsigned treshold = (N + 1) / 2; //split equally
				//the current node is full, creating a new node to hold the inserted key
				if(inner->num_keys == N) {

					new_sibling = new_inner_node();

					new_sibling->parent = inner->parent;

					if(child_sibling->num_keys == 0) {

						new_sibling->num_keys = 0;

						upKey = child_sibling->keys[N - 1];
						toInsert = new_sibling;
						k = -1;
					} else {

						//new_sibling should hold the excessive (>=threshold) keys
						new_sibling->num_keys = inner->num_keys - treshold;

						for(unsigned i = 0; i < new_sibling->num_keys; ++i) {
							new_sibling->keys[i] = inner->keys[treshold + i];
							new_sibling->children[i] = inner->children[treshold + i];
							reinterpret_cast<InnerNode*>(inner->children[treshold + i])->parent = new_sibling;
						}

						new_sibling->children[new_sibling->num_keys] = inner->children[inner->num_keys];
						reinterpret_cast<InnerNode*>(inner->children[inner->num_keys])->parent = new_sibling;

						//XXX: should threshold ???
						inner->num_keys = treshold - 1;

						upKey = inner->keys[treshold - 1];
						//after split, the new key could be inserted into the old node or the new node
						if(key >= upKey) {
							toInsert = new_sibling;
							if(k >= treshold) k = k - treshold;
							else k = 0;
						}
					}
					new_sibling->keys[N - 1] = upKey;
				}
				//inserting the new key to appropriate position
				if(k != -1) {
					for(int i = toInsert->num_keys; i > k; i--) {
						toInsert->keys[i] = toInsert->keys[i - 1];
						toInsert->children[i + 1] = toInsert->children[i];
					}
					toInsert->num_keys++;
					toInsert->keys[k] = child_sibling->keys[N - 1]; 

				}
				child_sibling->parent = toInsert;
				toInsert->children[k + 1] = child_sibling;
			}
		}

		if(d == depth && new_sibling != NULL) {
			InnerNode *new_root = new_inner_node();
			new_root->num_keys = 1;
			new_root->keys[0] = upKey;
			new_root->children[0] = root;
			new_root->children[1] = new_sibling;

			reinterpret_cast<InnerNode*>(root)->parent = new_root;
			new_sibling->parent = new_root;

			root = new_root;

			depth++;
		}
		return new_sibling; //return the newly-created node (if exists)
	}

	inline Memstore::MemNode* GetForRead(uint64_t key) {
		ThreadLocalInit();
		MemNode* value = Get(key);
#if DUMMY
		if(dummyval_ == NULL) {
			dummyval_ = GetMemNode();
		}
		if(dummyleaf_ == NULL) {
			dummyleaf_ = new LeafNode();
		}
#endif
		return value;
	}

	inline Memstore::MemNode* Insert_rtm(uint64_t key, LeafNode** target_leaf, bool should_sample = false) {
#if BTREE_LOCK
		MutexSpinLock lock(&slock);
#else
		RTMScope begtx(&prof, depth * 2, 1, &rtmlock, ADD_TYPE, should_sample);
#endif
		if(root == NULL) {
			LeafNode* root_leaf = new_leaf_node();
			
			for(int i = 0; i < SEGS; i++) {
				root_leaf->leaf_segs[i].max_room = EMP_LEN;
				root_leaf->leaf_segs[i].key_num = 0;
			}
			
			root_leaf->left = NULL;
			root_leaf->right = NULL;
			root_leaf->parent = NULL;
			root_leaf->seq = 0;
			root = root_leaf;
			depth = 0;
		}
		
		MemNode* val = NULL;
		if(depth == 0) {
			uint64_t upKey;
			LeafNode *new_leaf = LeafInsert(key, reinterpret_cast<LeafNode*>(root), &val, &upKey);

			if(new_leaf != NULL) { //a new leaf node is created, therefore adding a new inner node to hold
				InnerNode *inner = new_inner_node();

				inner->num_keys = 1;
				inner->keys[0] = upKey;
				inner->children[0] = root;
				inner->children[1] = new_leaf;
				reinterpret_cast<LeafNode*>(root)->parent = inner;
				new_leaf->parent = inner;
				depth++; //depth=1
				root = inner;


			}
		} else {
#if BTPREFETCH
			for(int i = 0; i <= 64; i += 64)
				prefetch(reinterpret_cast<char*>(root) + i);
#endif
			InnerInsert(key, reinterpret_cast<InnerNode*>(root), depth, &val, target_leaf);
		}
		return val;
	}

	void dump_leaf(LeafNode* leaf) {
		if(leaf==root){
			printf("I am root\n");
		}
		printf("leaf->kvs_num = %u\n" , leaf->kvs_num);
		for(int i = 0; i < leaf->kvs_num; i++) {
			printf("leaf->kvs[%d].key = %lu, value = %p\n", i, leaf->kvs[i].key, leaf->kvs[i].value);
		}
		for(int i = 0; i < SEGS; i++) {
			printf(
				"leaf->leaf_segs[%d].key_num = %u max_room = %u\n" , i, leaf->leaf_segs[i].key_num, leaf->leaf_segs[i].max_room);
			for(int j = 0; j < leaf->leaf_segs[i].key_num; j++) {
				printf("[num = %d] leaf->leaf_segs[%d].kvs[%d].key = %lu, value = %p\n", leaf->leaf_segs[i].key_num, i, j, leaf->leaf_segs[i].kvs[j].key,
					   leaf->leaf_segs[i].kvs[j].value);
			}
		}
	}

	void dump_inner(InnerNode* inner) {
		for(int i = 0 ; i < inner->num_keys; i++) {
			printf("inner->keys[%d] = %lu\n", i, inner->keys[i]);
		}
	}

	inline bool FindDuplicate(LeafNode* leaf, uint64_t key, MemNode** val) {
		for(int i = 0; i < SEGS; i++) {
			for(int j = 0; j < leaf->leaf_segs[i].key_num; j++) {
				if(leaf->leaf_segs[i].kvs[j].key == key) {
					*val = leaf->leaf_segs[i].kvs[j].value;
					return true;
				}
			}
		}
		for(int i = 0; i < leaf->kvs_num; i++) {
			if(leaf->kvs[i].key == key) {
				*val = leaf->kvs[i].value;
				return true;
			}
		}
		return false;
	}

	inline void ReorganizeLeafNode(LeafNode* leaf) {
		if(leaf->reserved == NULL) {
			leaf->reserved = (KeyValue*)calloc(LEAF_NUM, sizeof(KeyValue));
		} else {
			memset(leaf->reserved, 0, sizeof(KeyValue)*LEAF_NUM);
		}
		for(int i = 0; i < leaf->kvs_num; i++){
			leaf->reserved[i] = leaf->kvs[i];
		}
		
		int kvs_num = leaf->kvs_num;
		int temp = 0;
		int segment_keys = 0;
		for(int i = 0 ; i < SEGS; i++) {
			segment_keys += leaf->leaf_segs[i].key_num;
			for(int j = 0; j < leaf->leaf_segs[i].key_num; j++) {
				leaf->reserved[kvs_num + temp] = leaf->leaf_segs[i].kvs[j];
				temp++;
			}
		}
		std::sort(leaf->reserved, leaf->reserved + segment_keys+kvs_num, KVCompare);
		leaf->num_keys = segment_keys+kvs_num;
	}

	inline void dump_reserved(LeafNode* leaf) {
		printf("reserved[%p] = [", leaf);
		for(int i = 0; i < leaf->num_keys; i++) {
			printf("%lu, ", leaf->reserved[i].key);
		}
		printf("]\n");
	}

	//upKey should be the least key of the new LeafNode
	inline LeafNode* ShuffleLeafInsert(uint64_t key, LeafNode *leaf, MemNode** val, uint64_t* upKey, bool insert_only) {
#if DUP_PROF
		leaf_inserts++;
#endif
		LeafNode *new_sibling = NULL;
		if(!insert_only) {
			bool found = FindDuplicate(leaf, key, val); //if found, val is already set to the retrieved value
			if(found) { //duplicate insertion
#if	DUP_PROF
				dup_found++;
#endif
				return NULL;
			}
		}
		
		int idx = key % SEGS;
		int retries = 0;
		while (leaf->leaf_segs[idx].isFull() && retries < THRESHOLD) {
			idx = (idx << 1) % SEGS;			
			retries++;
		}
		//[Case #1] Shuffle to an empty segment, insert immediately
		if(!leaf->leaf_segs[idx].isFull() && !leaf->isFull()) {
			*val = leaf->insertSegment(idx, key);
			return NULL;
		} 
		else { 
			
			if(leaf->hasRoom()) {
				leaf->moveToReserved();
				leaf->shrinkSegs();
				*val = leaf->insertSegment(idx, key);
				return NULL;
			}
			else if(!leaf->isFull()) {
				leaf->moveToReserved();
				*val = leaf->insertSegment(idx, key);
				return NULL;
			}
			else { //REALLY FULL
#if DUP_PROF
				leaf_splits++;
#endif
				//[Case #3] This LeafNode is really full

				leaf->seq++; //increase the sequential number of split leaf to keep consistency
				
				//move the keys to reserved room
				leaf->moveToReserved();
				
				//sort the keys
				std::sort(leaf->kvs, leaf->kvs + LEAF_NUM, KVCompare);
				//insert and split
				unsigned k = 0;
				while((k < LEAF_NUM) && (leaf->kvs[k].key < key)) {
					++k;
				}

				LeafNode *toInsert = leaf;

				new_sibling = new_leaf_node(); //newly-born leafnode

				if(leaf->right == NULL && k == LEAF_NUM) {//new leafnode at rightmost
					for(int i = 0; i < SEGS; i++){
						leaf->leaf_segs[i].max_room=0;
						leaf->leaf_segs[i].key_num=0;
					}					
					leaf->kvs_num = LEAF_NUM;
					toInsert = new_sibling;
					if(new_sibling->leaf_segs[0].key_num != 0) {
						for(int i = 0; i < SEGS; i++) {
							new_sibling->leaf_segs[i].key_num = 0;
						}
					}
					for(int i = 0; i < SEGS; i++) {
						new_sibling->leaf_segs[i].key_num = 0;
						new_sibling->leaf_segs[i].max_room = EMP_LEN;
					}
					new_sibling->kvs_num = 0;
					toInsert->leaf_segs[0].key_num = 1;
					toInsert->leaf_segs[0].kvs[0].key = key;
					toInsert->kvs[0].key = key; //keys[0] should be set here
					*upKey = key; //the sole new key should be the upkey

				} else { //not at rightmost
					unsigned threshold = (LEAF_NUM + 1) / 2; //8
					//new_sibling->num_keys = leaf->num_keys - threshold;
					unsigned new_sibling_num_keys = LEAF_NUM - threshold; //8
					leaf->kvs_num = threshold;
					new_sibling->kvs_num = new_sibling_num_keys;
					//moving the keys above the threshold to the new sibling
					//new_sibling->born_key_num = new_sibling_num_keys; //new-born leaf

					//leaf->born_key_num = threshold; //old leaf

					for(int i = 0; i < SEGS; i++) {
						leaf->leaf_segs[i].key_num = 0;
						leaf->leaf_segs[i].max_room = HAL_LEN;
						new_sibling->leaf_segs[i].key_num = 0;
						new_sibling->leaf_segs[i].max_room = HAL_LEN;
					}

					for(int i = 0; i < new_sibling_num_keys; i++) {
						new_sibling->kvs[i] = leaf->kvs[threshold + i];
					}
					if(k >= threshold) {
						toInsert = new_sibling;
					}
					toInsert->leaf_segs[0].key_num = 1;
					toInsert->leaf_segs[0].kvs[0].key = key;

					if(k == threshold) {
						*upKey = key;
					} else {
						*upKey = new_sibling->kvs[0].key;
					}

				}
				//inserting the newsibling at the right of the old leaf node
				if(leaf->right != NULL) {
					leaf->right->left = new_sibling;
				}

				new_sibling->right = leaf->right;
				new_sibling->left = leaf;
				leaf->right = new_sibling;

				new_sibling->parent = leaf->parent;

#if DUMMY
				toInsert->leaf_segs[0].kvs[0].value = dummyval_;
				*val = dummyval_;
#else
				toInsert->leaf_segs[0].kvs[0].value = GetMemNode();
				*val = toInsert->leaf_segs[0].kvs[0].value;
#endif

				assert(*val != NULL);
				dummyval_ = NULL;
			}
		}
		return new_sibling;
	}

//Insert a key at the leaf level
//Return: the new node where the new key resides, NULL if no new node is created
//@val: storing the pointer to new value in val
	inline LeafNode* LeafInsert(uint64_t key, LeafNode *leaf, MemNode** val, uint64_t* upKey) {
#if SHUFFLE_INSERT
		return ShuffleLeafInsert(key, leaf, val, upKey, false);
#else
		return SimpleLeafInsert(key, leaf, val);
#endif
	}

	Memstore::Iterator* GetIterator() {
		return new MemstoreEunoTree::Iterator(this);
	}
	void printLeaf(LeafNode *n);
	void printInner(InnerNode *n, unsigned depth);
	void PrintStore();
	void PrintList();
	void checkConflict(int sig, int mode) ;

//YCSB TREE COMPARE Test Purpose
	void TPut(uint64_t key, uint64_t *value) {
#if BTREE_LOCK
		MutexSpinLock lock(&slock);
#else
		RTMScope begtx(&prof, depth * 2, 1, &rtmlock);
#endif

		if(root == NULL) {
			root = new_leaf_node();
			reinterpret_cast<LeafNode*>(root)->left = NULL;
			reinterpret_cast<LeafNode*>(root)->right = NULL;
			reinterpret_cast<LeafNode*>(root)->seq = 0;
			depth = 0;
		}

		if(depth == 0) {
			LeafNode *new_leaf = TLeafInsert(key, reinterpret_cast<LeafNode*>(root), value);
			if(new_leaf != NULL) {
				InnerNode *inner = new_inner_node();
				inner->num_keys = 1;
				inner->keys[0] = new_leaf->kvs[0].key;
				inner->children[0] = root;
				inner->children[1] = new_leaf;
				depth++;
				root = inner;
#if BTREE_PROF
				writes++;
#endif
			}
		} else {

#if BTPREFETCH
			for(int i = 0; i <= 64; i += 64)
				prefetch(reinterpret_cast<char*>(root) + i);
#endif

			TInnerInsert(key, reinterpret_cast<InnerNode*>(root), depth, value);
		}
	}

	inline LeafNode* TLeafInsert(uint64_t key, LeafNode *leaf, uint64_t *value) {
		LeafNode *new_sibling = NULL;
		unsigned k = 0;
		while((k < leaf->num_keys) && (leaf->kvs[k].key < key)) {
			++k;
		}

		if((k < leaf->num_keys) && (leaf->kvs[k].key == key)) {
			leaf->kvs[k].value = (Memstore::MemNode *)value;
			return NULL;
		}

		LeafNode *toInsert = leaf;
		if(leaf->num_keys == LEAF_NUM) {
			new_sibling = new_leaf_node();
			if(leaf->right == NULL && k == leaf->num_keys) {
				new_sibling->num_keys = 0;
				toInsert = new_sibling;
				k = 0;
			} else {
				unsigned threshold = (LEAF_NUM + 1) / 2;
				new_sibling->num_keys = leaf->num_keys - threshold;
				for(unsigned j = 0; j < new_sibling->num_keys; ++j) {
					new_sibling->kvs[j] = leaf->kvs[threshold + j];
				}
				leaf->num_keys = threshold;

				if(k >= threshold) {
					k = k - threshold;
					toInsert = new_sibling;
				}
			}
			if(leaf->right != NULL) leaf->right->left = new_sibling;
			new_sibling->right = leaf->right;
			new_sibling->left = leaf;
			leaf->right = new_sibling;
			new_sibling->seq = 0;
#if BTREE_PROF
			writes++;
#endif
		}

		for(int j = toInsert->num_keys; j > k; j--) {
			toInsert->kvs[j] = toInsert->kvs[j - 1];
		}

		toInsert->num_keys = toInsert->num_keys + 1;
		toInsert->kvs[k].key = key;
		toInsert->kvs[k].value = (Memstore::MemNode *)value;

		return new_sibling;
	}

	inline InnerNode* TInnerInsert(uint64_t key, InnerNode *inner, int d, uint64_t* value) {
		unsigned k = 0;
		uint64_t upKey;
		InnerNode *new_sibling = NULL;

		while((k < inner->num_keys) && (key >= inner->keys[k])) {
			++k;
		}

		void *child = inner->children[k];

#if BTPREFETCH
		for(int i = 0; i <= 64; i += 64)
			prefetch(reinterpret_cast<char*>(child) + i);
#endif

		if(d == 1) {
			LeafNode *new_leaf = TLeafInsert(key, reinterpret_cast<LeafNode*>(child), value);
			if(new_leaf != NULL) {
				InnerNode *toInsert = inner;
				if(inner->num_keys == N) {
					new_sibling = new_inner_node();
					if(new_leaf->num_keys == 1) {
						new_sibling->num_keys = 0;
						upKey = new_leaf->kvs[0].key;
						toInsert = new_sibling;
						k = -1;
					} else {
						unsigned treshold = (N + 1) / 2;
						new_sibling->num_keys = inner->num_keys - treshold;

						for(unsigned i = 0; i < new_sibling->num_keys; ++i) {
							new_sibling->keys[i] = inner->keys[treshold + i];
							new_sibling->children[i] = inner->children[treshold + i];
						}

						new_sibling->children[new_sibling->num_keys] = inner->children[inner->num_keys];
						inner->num_keys = treshold - 1;

						upKey = inner->keys[treshold - 1];

						if(new_leaf->kvs[0].key >= upKey) {
							toInsert = new_sibling;
							if(k >= treshold) k = k - treshold;
							else k = 0;
						}
					}
					new_sibling->keys[N - 1] = upKey;
				}

				if(k != -1) {
					for(int i = toInsert->num_keys; i > k; i--) {
						toInsert->keys[i] = toInsert->keys[i - 1];
						toInsert->children[i + 1] = toInsert->children[i];
					}
					toInsert->num_keys++;
					toInsert->keys[k] = new_leaf->kvs[0].key;
				}
				toInsert->children[k + 1] = new_leaf;
			}

		} else {
			bool s = true;
			InnerNode *new_inner =
				TInnerInsert(key, reinterpret_cast<InnerNode*>(child), d - 1, value);

			if(new_inner != NULL) {
				InnerNode *toInsert = inner;
				InnerNode *child_sibling = new_inner;


				unsigned treshold = (N + 1) / 2;
				if(inner->num_keys == N) {
					new_sibling = new_inner_node();

					if(child_sibling->num_keys == 0) {
						new_sibling->num_keys = 0;
						upKey = child_sibling->keys[N - 1];
						toInsert = new_sibling;
						k = -1;
					}

					else  {
						new_sibling->num_keys = inner->num_keys - treshold;

						for(unsigned i = 0; i < new_sibling->num_keys; ++i) {
							new_sibling->keys[i] = inner->keys[treshold + i];
							new_sibling->children[i] = inner->children[treshold + i];
						}
						new_sibling->children[new_sibling->num_keys] =
							inner->children[inner->num_keys];


						inner->num_keys = treshold - 1;

						upKey = inner->keys[treshold - 1];
						if(key >= upKey) {
							toInsert = new_sibling;
							if(k >= treshold) k = k - treshold;
							else k = 0;
						}
					}
					new_sibling->keys[N - 1] = upKey;


				}
				if(k != -1) {
					for(int i = toInsert->num_keys; i > k; i--) {
						toInsert->keys[i] = toInsert->keys[i - 1];
						toInsert->children[i + 1] = toInsert->children[i];
					}

					toInsert->num_keys++;
					toInsert->keys[k] = reinterpret_cast<InnerNode*>(child_sibling)->keys[N - 1];
				}
				toInsert->children[k + 1] = child_sibling;
			}
		}

		if(d == depth && new_sibling != NULL) {
			InnerNode *new_root = new_inner_node();
			new_root->num_keys = 1;
			new_root->keys[0] = upKey;
			new_root->children[0] = root;
			new_root->children[1] = new_sibling;
			root = new_root;
			depth++;
		}
		return new_sibling;
	}

public:
	static __thread RTMArena* arena_;	  // Arena used for allocations of nodes
	static __thread bool localinit_;
	static __thread MemNode *dummyval_;
	static __thread LeafNode *dummyleaf_;

	void *root;
	int depth;

	RTMProfile delprof;

	RTMProfile prof;
	RTMProfile prof1;
	RTMProfile prof2;
	port::SpinLock slock;
#if BTREE_PROF
public:
	uint64_t reads;
	uint64_t writes;
	uint64_t calls;
#endif
	SpinLock rtmlock;

	int current_tid;
	int windex[4];
	int rindex[4];
};

//__thread RTMArena* MemstoreEunoTree::arena_ = NULL;
//__thread bool MemstoreEunoTree::localinit_ = false;
}
#endif
