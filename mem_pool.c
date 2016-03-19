#include <stdlib.h>
#include <assert.h>
#include <stdio.h> // for perror()

#include "mem_pool.h"

/*************/
/*           */
/* Constants */
/*           */
/*************/
static const float      MEM_FILL_FACTOR                 = 0.75;
static const unsigned   MEM_EXPAND_FACTOR               = 2;

static const unsigned   MEM_POOL_STORE_INIT_CAPACITY    = 20;
static const float      MEM_POOL_STORE_FILL_FACTOR      = 0.75;
static const unsigned   MEM_POOL_STORE_EXPAND_FACTOR    = 2;

static const unsigned   MEM_NODE_HEAP_INIT_CAPACITY     = 40;
static const float      MEM_NODE_HEAP_FILL_FACTOR       = 0.75;
static const unsigned   MEM_NODE_HEAP_EXPAND_FACTOR     = 2;

static const unsigned   MEM_GAP_IX_INIT_CAPACITY        = 40;
static const float      MEM_GAP_IX_FILL_FACTOR          = 0.75;
static const unsigned   MEM_GAP_IX_EXPAND_FACTOR        = 2;

/*********************/
/*                   */
/* Type declarations */
/*                   */
/*********************/
typedef struct _node {
    alloc_t alloc_record;
    unsigned used;
    unsigned allocated;
    struct _node *next, *prev; // doubly-linked list for gap deletion
} node_t, *node_pt;

typedef struct _gap {
    size_t size;
    node_pt node;
} gap_t, *gap_pt;

typedef struct _pool_mgr {
    pool_t pool;
    node_pt node_heap;
    unsigned total_nodes;
    unsigned used_nodes;
    gap_pt gap_ix;
    unsigned gap_ix_capacity;
} pool_mgr_t, *pool_mgr_pt;

/***************************/
/*                         */
/* Static global variables */
/*                         */
/***************************/
static pool_mgr_pt *pool_store = NULL; // an array of pointers, only expand
static unsigned pool_store_size = 0;
static unsigned pool_store_capacity = 0;

/********************************************/
/*                                          */
/* Forward declarations of static functions */
/*                                          */
/********************************************/
static alloc_status _mem_resize_pool_store();
static alloc_status _mem_resize_node_heap(pool_mgr_pt pool_mgr);
static alloc_status _mem_resize_gap_ix(pool_mgr_pt pool_mgr);
static alloc_status
        _mem_add_to_gap_ix(pool_mgr_pt pool_mgr,
                           size_t size,
                           node_pt node);
static alloc_status
        _mem_remove_from_gap_ix(pool_mgr_pt pool_mgr,
                                size_t size,
                                node_pt node);
static alloc_status _mem_sort_gap_ix(pool_mgr_pt pool_mgr);


/****************************************/
/*                                      */
/* Definitions of user-facing functions */
/*                                      */
/****************************************/
alloc_status mem_init() {

    // ensure that it's called only once until mem_free
    // allocate the pool store with initial capacity
    // note: holds pointers only, other functions to allocate/deallocate
    if ( pool_store == NULL) {
        pool_store = (pool_mgr_pt*)calloc( MEM_POOL_STORE_INIT_CAPACITY , sizeof(pool_mgr_pt) );
        pool_store_capacity = MEM_POOL_STORE_INIT_CAPACITY;
        pool_store_size = 0;
    }
    else
        return ALLOC_CALLED_AGAIN;
    return ALLOC_OK;
}

alloc_status mem_free() {
    // ensure that it's called only once for each mem_init
    if (pool_store ==NULL)
        return ALLOC_CALLED_AGAIN;
    // make sure all pool managers have been deallocated
    // can free the pool store array
    // update static variables
    // deallocate every pool memory
    for ( int i = 0; i < pool_store_capacity ; i ++) {
        pool_store[i] = NULL;
    }
    // free pool
    free(pool_store);

    // update status after deallocated
    pool_store_capacity = 0;
    pool_store_size = 0;
    pool_store = NULL;

    return ALLOC_OK;
}

pool_pt mem_pool_open(size_t size, alloc_policy policy) {
    // make sure there the pool store is allocated
    assert(pool_store);
    if (pool_store == NULL)
        return NULL;
    // expand the pool store, if necessary
    _mem_resize_pool_store();

    // allocate a new mem pool mgr
    pool_mgr_pt pool_mgr =(pool_mgr_pt) calloc(1, sizeof(pool_mgr_t) );
    // check success, on error return null
    assert(pool_mgr);
    if ( pool_mgr == NULL) {
        return NULL;
    }
    // allocate a new memory pool
    pool_mgr->pool.mem = (char*) calloc(size, sizeof(char));
    // check success, on error deallocate mgr and return null
    assert(pool_mgr->pool.mem);
    if ( pool_mgr->pool.mem == NULL) {
        free(pool_mgr);
        return NULL;
    }
    // allocate a new node heap
    pool_mgr->node_heap = (node_pt) calloc(MEM_NODE_HEAP_INIT_CAPACITY, sizeof(node_t));
    // check success, on error deallocate mgr/pool and return null
    assert(pool_mgr->node_heap);
    if ( pool_mgr->node_heap == NULL) {
        free(pool_mgr->pool.mem);
        free(pool_mgr);
        return NULL;
    }
    // allocate a new gap index
    pool_mgr->gap_ix =(gap_pt) calloc(MEM_GAP_IX_INIT_CAPACITY, sizeof(gap_t));
    for ( int i=0; i < MEM_GAP_IX_INIT_CAPACITY; i ++)
        pool_mgr->gap_ix[i].size = 0;
    // check success, on error deallocate mgr/pool/heap and return null
    assert(pool_mgr->gap_ix);
    if ( pool_mgr->gap_ix == NULL) {
        free(pool_mgr->pool.mem);
        free(pool_mgr->node_heap);
        free(pool_mgr);
        return NULL;
    }
    // assign all the pointers and update meta data:
    pool_mgr->pool.total_size = size;
    pool_mgr->pool.alloc_size= 0;
    pool_mgr->pool.num_gaps = 1;
    pool_mgr->pool.policy = policy;
    pool_mgr->pool.num_allocs= 0;

    //   initialize top node of node heap
    pool_mgr->node_heap[0].alloc_record.mem = pool_mgr->pool.mem;
    pool_mgr->node_heap[0].alloc_record.size = size;
    pool_mgr->node_heap[0].allocated = 0;
    pool_mgr->node_heap[0].next = NULL;
    pool_mgr->node_heap[0].prev = NULL;
    pool_mgr->node_heap[0].used = 1;


    //   initialize top node of gap index
    pool_mgr->gap_ix[0].size = pool_mgr->node_heap->alloc_record.size;
    pool_mgr->gap_ix[0].node = pool_mgr->node_heap;

    //   initialize pool mgr
    // pool_mgr->gap_ix_capacity = MEM_GAP_IX_INIT_CAPACITY;
    pool_mgr->total_nodes = MEM_NODE_HEAP_INIT_CAPACITY;
    pool_mgr->used_nodes = 1;
    pool_mgr->gap_ix_capacity = MEM_GAP_IX_INIT_CAPACITY;
    //   link pool mgr to pool store
    // return the address of the mgr, cast to (pool_pt)
    //unsigned int i;
    //while (pool_store[i] != NULL) {
    //    ++i;
    //}
    pool_store[pool_store_size] = pool_mgr;
//    pool_mgr->pool.num_allocs= 0;
//    pool_mgr->pool.num_gaps = 1;
//     return the address of the mgr, cast to (pool_pt)
    pool_store_size ++;
    return (pool_pt) pool_mgr;
}

alloc_status mem_pool_close(pool_pt pool) {
    // get mgr from pool by casting the pointer to (pool_mgr_pt)
    pool_mgr_pt pool_mgr = (pool_mgr_pt) pool;
    // check if this pool is allocated
    if ( pool_mgr->pool.mem == NULL || pool_mgr->pool.num_gaps >1 || pool_mgr->pool.num_allocs >0) {
        return ALLOC_NOT_FREED;
    }
    // check if pool has only one gap
    // check if it has zero allocations

    // free memory pool
    free(pool_mgr->pool.mem);
    pool_mgr->pool.mem = NULL;
    // free node heap
    free(pool_mgr->node_heap);
    pool_mgr->node_heap = NULL;
    // free gap index
    free(pool_mgr->gap_ix);
    pool_mgr->gap_ix =NULL;
    // find mgr in pool store and set to null

    for ( int i = 0; i< pool_store_capacity; i++){
        if (pool_mgr == pool_store[i]) {
            pool_store[i] = NULL;
            break;
        }
    }
    // note: don't decrement pool_store_size, because it only grows
    // free mgr
    free(pool_mgr);
    pool_mgr = NULL;

    return ALLOC_OK;
}

alloc_pt mem_new_alloc(pool_pt pool, size_t size) {
    // get mgr from pool by casting the pointer to (pool_mgr_pt)
    pool_mgr_pt pool_mgr = (pool_mgr_pt) pool;
    // check if any gaps, return null if none
    alloc_status status = (pool ==NULL);
    assert(status == ALLOC_OK);
    if (pool_mgr->pool.num_gaps == 0) {
        return NULL;
    }

    // alloc_status status = _mem_resize_node_heap(pool_mgr);
    //if ( status!= ALLOC_OK)
    //    return NULL;

    // expand heap node, if necessary, quit on error
    // assert(_mem_resize_node_heap(pool_mgr)==ALLOC_OK);

    // check used nodes fewer than total nodes, quit on error
    //assert(pool_mgr->used_nodes >= pool_mgr->total_nodes);
    if (pool_mgr->used_nodes >= pool_mgr->total_nodes) {
        return NULL;
    }

    // must provide both cases.
    // sort from node heap ... same treverse thru a whole list of node heap.
    // best fit  sort form gap index , treverse thru a whole list and first one. ...
    // if FIRST_FIT, then find the first sufficient node in the node heap
    node_pt new_node =NULL ;
    if ( pool_mgr->pool.policy == FIRST_FIT){
        for ( int i = 0 ; i < pool_mgr->total_nodes; i ++ ) {
            if ((pool_mgr->node_heap[i].allocated == 0) && (pool_mgr->node_heap[i].alloc_record.size >= size)) {
                new_node = &pool_mgr->node_heap[i];
                break;
            }
        }
    }
    if(pool_mgr->pool.policy == BEST_FIT) {
        // if BEST_FIT, then find the first sufficient node in the gap index
        for (int i = 0; i <pool_mgr->pool.num_gaps; i++) {
            if ((pool_mgr->gap_ix[i].node->allocated == 0) && (pool_mgr->gap_ix[i].node->alloc_record.size >= size ) ) {
                new_node = pool_mgr->gap_ix[i].node;
                break;
            }
        }
    }
    // check if node found
    //assert(new_node);
    if (new_node == NULL) {
        return NULL;
    }
    // get a node for allocation:
    // update metadata (num_allocs, alloc_size)
    pool->num_allocs ++;
    pool->alloc_size +=size;
    // calculate the size of the remaining gap, if any
    size_t remaining_gap = new_node->alloc_record.size -size;
    // remove node from gap index
    _mem_remove_from_gap_ix(pool_mgr,size,new_node);
    // convert gap_node to an allocation node of given size
    new_node->allocated =1;
   // new_node->used =1;
    new_node->alloc_record.size =size;
    // adjust node heap:
    node_pt unused_node;
    //   if remaining gap, need a new node
    if ( remaining_gap !=0) {
        //   find an unused one in the node heap
        //   make sure one was found
        // find unused node
        for (int i = 0; i < pool_mgr->total_nodes; i++) {
            if (pool_mgr->node_heap[i].used == 0) {
                unused_node = &pool_mgr->node_heap[i];
                break;
            }
        }
        if  ( unused_node ==NULL)
            return NULL;
        //   initialize it to a gap node
        //   update metadata (used_nodes)
        unused_node->allocated = 0;
        unused_node->used = 1;
        unused_node->alloc_record.mem = new_node->alloc_record.mem + size;
        unused_node->alloc_record.size = remaining_gap;
        pool_mgr->used_nodes++;

        //   update linked list (new node right after the node for allocation)
        //   add to gap index
        //   check if successful
        unused_node ->prev = new_node;

        if (new_node->next == NULL) {
            new_node->next = unused_node;
            unused_node->next = NULL;
        }
        else{
            unused_node->next = new_node->next;
            new_node->next->prev = unused_node;
            new_node->next = unused_node;

        }


        //_mem_add_to_gap_ix(pool_mgr, unused_node->alloc_record.size, unused_node);
        // return allocation record by casting the node to (alloc_pt)

        alloc_status status = _mem_add_to_gap_ix(pool_mgr, remaining_gap, unused_node);
        assert(status == ALLOC_OK);
    }
    if (new_node == NULL)
        return NULL;

    return (alloc_pt) new_node;
}

// TODO DONE DONE DONE !!!!
alloc_status mem_del_alloc(pool_pt pool, alloc_pt alloc) {
    // get mgr from pool by casting the pointer to (pool_mgr_pt)
    pool_mgr_pt pool_mgr = (pool_mgr_pt) pool;
    // get node from alloc by casting the pointer to (node_pt)
    node_pt new_node = (node_pt) alloc;
    // find the node in the node heap
    // this is node-to-delete
    node_pt node_to_delete;
    for ( int i =0; i < pool_mgr->total_nodes; i++) {
        if ((new_node->alloc_record.mem) == (pool_mgr->node_heap[i].alloc_record.mem)){
            node_to_delete = &pool_mgr->node_heap[i];
            break;
        }
    }
    // make sure it's found
    if ( node_to_delete == NULL)
        return ALLOC_NOT_FREED;
    // convert to gap node
    node_to_delete->allocated = 0;
    // update metadata (num_allocs, alloc_size)
    pool_mgr->pool.num_allocs --;
    pool_mgr->pool.alloc_size -= node_to_delete->alloc_record.size;
    // if the next node in the list is also a gap, merge into node-to-delete
    //   remove the next node from gap index
    //   check success
    //   add the size to the node-to-delete
    //   update node as unused
    node_pt next_node;
    if ((node_to_delete->next !=NULL) && (node_to_delete->next->allocated == 0)
        && (node_to_delete->next->used =1)) {
        next_node = node_to_delete->next;
        //next_node->alloc_record.size = node_to_delete->alloc_record.size;
        _mem_remove_from_gap_ix(pool_mgr, next_node->alloc_record.size, next_node);
        node_to_delete->alloc_record.size +=  node_to_delete->next->alloc_record.size;
        node_to_delete->next->used = 0;
        node_to_delete->next->alloc_record.size = 0;
        node_to_delete->next->alloc_record.mem = NULL;
        //node_to_delete->used = 0;
        next_node->used = 0;
        //   update metadata (used nodes)
        pool_mgr->used_nodes--;
        //   update linked list:
        /*
         if (next->next) {
         next->next->prev = node_to_del;
         node_to_del->next = next->next;
         } else {
         node_to_del->next = NULL;
         }
         next->next = NULL;
         next->prev = NULL;
         */
        if (next_node->next) {
            next_node->next->prev = node_to_delete;
            node_to_delete->next = next_node->next;
        }
        else {
            node_to_delete->next = NULL;
        }
        next_node->next = NULL;
        next_node->prev = NULL;
    }
    _mem_add_to_gap_ix(pool_mgr,node_to_delete->alloc_record.size, node_to_delete);

    // this merged node-to-delete might need to be added to the gap index
    // but one more thing to check...
    // if the previous node in the list is also a gap, merge into previous!
    //   remove the previous node from gap index
    //   check success
    //   add the size of node-to-delete to the previous
    //   update node-to-delete as unused
    node_pt pre_node;
    if((node_to_delete->prev != NULL) &&(node_to_delete->prev->allocated == 0)
       && ( node_to_delete->prev->used =1) ) {
        pre_node = node_to_delete->prev;
        alloc_status status = _mem_remove_from_gap_ix(pool_mgr, pre_node->alloc_record.size, pre_node);
        if (status == ALLOC_FAIL)
            return ALLOC_FAIL;
        status = _mem_remove_from_gap_ix(pool_mgr, node_to_delete->alloc_record.size, node_to_delete);
        if (status == ALLOC_FAIL)
            return ALLOC_FAIL;
        pre_node->alloc_record.size = node_to_delete->alloc_record.size + node_to_delete->prev->alloc_record.size;
        node_to_delete->used = 0;
        node_to_delete->alloc_record.size = 0;
        node_to_delete->alloc_record.mem = NULL;

        pool_mgr->used_nodes--;

        //   update metadata (used_nodes)
        //   update linked list
        /*
         if (node_to_del->next) {
         prev->next = node_to_del->next;
         node_to_del->next->prev = prev;
         } else {
         prev->next = NULL;
         }
         node_to_del->next = NULL;
         node_to_del->prev = NULL;
         */

        if (node_to_delete->next) {
            pre_node->next = node_to_delete->next;
            node_to_delete->next->prev = pre_node;
        }
        else {
            pre_node->next = NULL;
        }
        node_to_delete->next = NULL;
        node_to_delete->prev = NULL;
        //node_to_delete = pre_node;

        //   change the node to add to the previous node!
        // add the resulting node to the gap index
        // check success

        _mem_add_to_gap_ix(pool_mgr, pre_node->alloc_record.size, pre_node);
    }
    return ALLOC_OK;
}

void mem_inspect_pool(pool_pt pool,
                      pool_segment_pt *segments,
                      unsigned *num_segments) {
    // get the mgr from the pool
    pool_mgr_pt pool_mgr = (pool_mgr_pt) pool;
    // allocate the segments array with size == used_nodes
    pool_segment_pt segs = (pool_segment_pt ) calloc(pool_mgr->used_nodes, sizeof(pool_segment_t));
    // check successful
    //assert(segs);
    if (segs == NULL)
        return;


    // loop through the node heap and the segments array
    //    for each node, write the size and allocated in the segment
    // "return" the values:


    node_pt  nodeCheck =pool_mgr->node_heap;

    for ( int i = 0; i < pool_mgr->used_nodes;i++) {
        segs[i].size = nodeCheck->alloc_record.size;
        segs[i].allocated = nodeCheck->allocated;
        if ( nodeCheck->next != NULL)
            nodeCheck= nodeCheck->next;
    }
    /*
    *segments = segs;
    *num_segments = pool_mgr->used_nodes;
    */
    *segments = segs;
    *num_segments = pool_mgr->used_nodes;
}



/***********************************/
/*                                 */
/* Definitions of static functions */
/*                                 */
/***********************************/

static alloc_status _mem_resize_pool_store() {
    // check if necessary
    // don't forget to update capacity variables
    if (((float) pool_store_size / pool_store_capacity) > MEM_POOL_STORE_FILL_FACTOR) {
        pool_store = realloc(pool_store, MEM_EXPAND_FACTOR  * sizeof(pool_store));
        pool_store_capacity *= MEM_EXPAND_FACTOR;
    }
    return ALLOC_OK;

}
static alloc_status _mem_resize_node_heap(pool_mgr_pt pool_mgr) {
    if (((float) pool_mgr->node_heap->used / pool_mgr->node_heap->alloc_record.size) > MEM_NODE_HEAP_FILL_FACTOR) {

        pool_mgr->node_heap = (node_pt) realloc(pool_mgr->node_heap,MEM_NODE_HEAP_EXPAND_FACTOR * sizeof(pool_store));
        pool_mgr->total_nodes *= MEM_NODE_HEAP_EXPAND_FACTOR;
        for (int i = pool_mgr->total_nodes; i < pool_mgr->total_nodes; ++i) {
            pool_mgr->node_heap[i].used = 0;
            pool_mgr->node_heap[i].allocated = 0;
            pool_mgr->node_heap[i].alloc_record.size = 0;
            pool_mgr->node_heap[i].alloc_record.mem = NULL;
            pool_mgr->node_heap[i].next = NULL;
            pool_mgr->node_heap[i].prev = NULL;
        }

        //    if (((float) pool_store_size / pool_store_capacity) > MEM_POOL_STORE_FILL_FACTOR) {
        //    }
        // don't forget to update capacity variables
    }
    return ALLOC_OK;
}


static alloc_status _mem_resize_gap_ix(pool_mgr_pt pool_mgr) {
    if (((float) pool_mgr->pool.num_gaps / pool_mgr->gap_ix_capacity) > MEM_GAP_IX_FILL_FACTOR) {

        pool_mgr->gap_ix = (gap_pt) realloc(pool_mgr->gap_ix, MEM_GAP_IX_EXPAND_FACTOR * sizeof(pool_store));
        pool_mgr->gap_ix_capacity *= MEM_GAP_IX_EXPAND_FACTOR;
        for (int i = pool_mgr->pool.num_gaps; i < pool_mgr->gap_ix_capacity; ++i)
        {
            pool_mgr->gap_ix[i].size = 0;
            pool_mgr->gap_ix[i].node = NULL;
        }
    }
    //    if (((float) pool_store_size / pool_store_capacity) > MEM_POOL_STORE_FILL_FACTOR) {
    //    }
    // don't forget to update capacity variables
    return ALLOC_OK;
}

static alloc_status _mem_add_to_gap_ix(pool_mgr_pt pool_mgr,
                                       size_t size,
                                       node_pt node) {

    // expand the gap index, if necessary (call the function)
    _mem_resize_gap_ix(pool_mgr);

    // add the entry at the end
    pool_mgr->gap_ix[pool_mgr->pool.num_gaps].node = node;
    pool_mgr->gap_ix[pool_mgr->pool.num_gaps].size = size;
    // update metadata (num_gaps)
    pool_mgr->pool.num_gaps ++;
    // sort the gap index (call the function)
    alloc_status status = (_mem_sort_gap_ix(pool_mgr)== ALLOC_OK);
        // check success
    if (status)
        return ALLOC_OK;

    return ALLOC_FAIL;
}

static alloc_status _mem_remove_from_gap_ix(pool_mgr_pt pool_mgr,
                                            size_t size,
                                            node_pt node) {
    // find the position of the node in the gap index

    int index = 0;
    for ( int i = 0; i < pool_mgr->pool.num_gaps; i++) {
        if ( pool_mgr->gap_ix[i].node == node) {
            index = i;
            break;
        }
    }
    // loop from there to the end of the array:
    //    pull the entries (i.e. copy over) one position up
    //    this effectively deletes the chosen node
    for ( int i = index; i < pool_mgr->gap_ix_capacity - 1; i++) {
        pool_mgr->gap_ix[i] = pool_mgr->gap_ix[i+1];
    }
    // update metadata (num_gaps)
    pool_mgr->pool.num_gaps --;
    pool_mgr->gap_ix_capacity --;
    pool_mgr->gap_ix[pool_mgr->pool.num_gaps].size = 0;
    pool_mgr->gap_ix[pool_mgr->pool.num_gaps].node = NULL;
    // zero out the element at position num_gaps!

    return ALLOC_OK;
}

static alloc_status _mem_sort_gap_ix(pool_mgr_pt pool_mgr) {
    gap_t temp;
    // the new entry is at the end, so "bubble it up"
    // loop from num_gaps - 1 until but not including 0:
    //    if the size of the current entry is less than the previous (u - 1)
    //       swap them (by copying) (remember to use a temporary variable)
    for ( int i = (pool_mgr->pool.num_gaps) -1; i>0; i--) {
        if ( pool_mgr->gap_ix[i].node->alloc_record.size < pool_mgr->gap_ix[i-1].node->alloc_record.size ||
             pool_mgr->gap_ix[i].node->alloc_record.mem < pool_mgr->gap_ix[i-1].node->alloc_record.mem   ) {
            temp = pool_mgr->gap_ix[i-1];
            pool_mgr->gap_ix[i-1]= pool_mgr->gap_ix[i];
            pool_mgr->gap_ix[i] = temp;
        }
    }
    return ALLOC_OK;
}
