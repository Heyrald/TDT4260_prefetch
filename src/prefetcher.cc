/*
 * A sample prefetcher which does sequential one-block lookahead.
 * This means that the prefetcher fetches the next block _after_ the one that
 * was just accessed. It also ignores requests to blocks already in the cache.
 */

#include "interface.hh"

#include <list>
#include <stack>
#include <deque> //Circular buffer impl. with pop/push functionality
#include <algorithm>
#include <iostream>
#include <stdint.h>
#include "base/trace.hh"

using namespace std;



#define ENTRY_LIMIT 100
#define DELTAS_ENTRY 6
#define DELTA_BITFIELD_WIDTH 10
#define MAX_DELTA ((1 << ((DELTA_BITFIELD_WIDTH)-1)) - 1)
#define MIN_DELTA 0
#define INFLIGHT_ELEMENTS 32
#define DEBUG 0
typedef uint64_t delta_t;


//Global vars:
deque<Addr> In_flight;



struct dcpt_table_entry_s
{
    // Constructor:
    dcpt_table_entry_s(Addr PC, Addr LAST_ADDRESS, Addr LAST_PREFETCH) : pc(PC), last_address(LAST_ADDRESS), last_prefetch(LAST_PREFETCH){}

    //Vars:
    Addr pc;
    Addr last_address;
    Addr last_prefetch;
    deque<delta_t> delta_array;  //Circular buffer. Eliminates the need of the delta pointer.
};

struct dcpt_table_s
{
    //Const:
    dcpt_table_s() : num_entries_in_table(0) {}

    //Vars:
    int num_entries_in_table;
    list<dcpt_table_entry_s *> entries;
    
    //Function declarations:
    dcpt_table_entry_s *find_entry(Addr PC);
};

dcpt_table_entry_s *dcpt_table_s::find_entry(Addr PC)
{
    list<dcpt_table_entry_s *>::iterator i = entries.begin();

    for(; i != entries.end(); ++i)
    {
        dcpt_table_entry_s *table_entry = *i;
        if(PC == table_entry->pc)
            return table_entry; //Found entry in table, returns entry
    }
    return NULL;    //Found nothing
}

dcpt_table_s *dcpt_table;

list<Addr> delta_correlation(dcpt_table_entry_s *dcpt_table_entry)
{
    list<Addr> candidates;

    deque<delta_t>::iterator delta_array_iterator = dcpt_table_entry->delta_array.end();
    delta_t d1 = *(--delta_array_iterator);
    delta_t d2 = *(--delta_array_iterator);

    Addr address = dcpt_table_entry->last_address;

    delta_array_iterator = dcpt_table_entry->delta_array.begin();

    while(delta_array_iterator != dcpt_table_entry->delta_array.end())
    {
        delta_t u = *(delta_array_iterator++);
        delta_t v = *delta_array_iterator;

        if(u == d2 && v == d1)
            for(delta_array_iterator++; delta_array_iterator != dcpt_table_entry->delta_array.end(); ++delta_array_iterator)
            {
                address += *delta_array_iterator * BLOCK_SIZE;
                candidates.push_back(address);
            }
    }
    if(DEBUG >= 2)
        cout << "Num in candidates: " << candidates.size() << endl;
    return candidates;
}

list<Addr> prefetch_filter(dcpt_table_entry_s *dcpt_table_entry, list<Addr> candidates)
{
    list<Addr> prefetches;

    list<Addr>::iterator candidates_iterator;
    for(candidates_iterator = candidates.begin(); candidates_iterator != candidates.end(); ++candidates_iterator)
    {
        if(DEBUG >= 1)
            cout << "Finding prefetch ";
        if(std::find(In_flight.begin(), In_flight.end(), *candidates_iterator) == In_flight.end() && !in_mshr_queue(*candidates_iterator) && !in_cache(*candidates_iterator))
        {
            if(DEBUG >= 1)
                cout << "Pushed prefetcher" << endl;
            prefetches.push_back(*candidates_iterator);

            if(In_flight.size() == INFLIGHT_ELEMENTS)
            {
                In_flight.pop_front();
            }
            In_flight.push_back(*candidates_iterator);
            dcpt_table_entry->last_prefetch = *candidates_iterator;
        }
    }
    return prefetches;
}

void prefetch_init(void)
{
    dcpt_table = new dcpt_table_s;

    DPRINTF(HWPrefetch, "DCPT prefetcher init.\n");

}

void prefetch_access(AccessStat stat)
{
    int64_t delta;

    dcpt_table_entry_s *current_dcpt_entry = dcpt_table->find_entry(stat.pc);

    if(current_dcpt_entry == NULL)
    {
        if(dcpt_table->num_entries_in_table == ENTRY_LIMIT)
            dcpt_table->entries.pop_back();

        dcpt_table_entry_s *new_dcpt_entry = new dcpt_table_entry_s(stat.pc, stat.mem_addr, 0);
        //new_dcpt_entry->pc = stat.pc;
        //new_dcpt_entry->last_address = stat.mem_addr;
        //new_dcpt_entry->last_prefetch = 0;
        new_dcpt_entry->delta_array.push_front(1);
        dcpt_table->entries.push_front(new_dcpt_entry);
        current_dcpt_entry = new_dcpt_entry;

        if(dcpt_table->num_entries_in_table < ENTRY_LIMIT)
            ++dcpt_table->num_entries_in_table;
    }
    else
    {
        delta = ((int64_t)stat.mem_addr - (int64_t)current_dcpt_entry->last_address) / BLOCK_SIZE >> 1;

        if(delta != 0)
        {
            if(delta < MIN_DELTA || delta > MAX_DELTA)
                delta = MIN_DELTA;

            if(current_dcpt_entry->delta_array.size() == DELTAS_ENTRY)
                current_dcpt_entry->delta_array.pop_front();

            current_dcpt_entry->delta_array.push_back((delta_t)delta);
            current_dcpt_entry->last_address = stat.mem_addr;
        }
        list<Addr> prefetches = prefetch_filter(current_dcpt_entry, delta_correlation(current_dcpt_entry));

        if(DEBUG >= 1)
            cout << "Issuiung ";

        list<Addr>::iterator prefetch_iterator;
        for(prefetch_iterator = prefetches.begin(); prefetch_iterator != prefetches.end(); ++prefetch_iterator)
        {
            if(DEBUG >= 1)
                cout << "Fetching address";
            issue_prefetch(*prefetch_iterator);
        }
        if(DEBUG >= 1)
            cout << endl;
    }
}

void prefetch_complete(Addr addr) {
    DPRINTF(HWPrefetch, "Prefetch complete");
}
