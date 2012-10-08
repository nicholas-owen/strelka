// -*- mode: c++; indent-tabs-mode: nil; -*-
//
// Copyright (c) 2009-2012 Illumina, Inc.
//
// This software is covered by the "Illumina Genome Analyzer Software
// License Agreement" and the "Illumina Source Code License Agreement",
// and certain third party copyright/licenses, and any user of this
// source file is bound by the terms therein (see accompanying files
// Illumina_Genome_Analyzer_Software_License_Agreement.pdf and
// Illumina_Source_Code_License_Agreement.pdf and third party
// copyright/license notices).
//
//

/// \file
///
/// \author Chris Saunders
///

#ifndef __INDEL_BUFFER_HH
#define __INDEL_BUFFER_HH

#include "starling_common/indel.hh"

#include <iosfwd>
#include <map>


// TODO -- should not pass raw iterators to client code, instead need a
// a custom iterator to reliably lookup the indel_data object
//


// holds the complete set of candidate indels in buffer
//
// TODO make structure more efficient by only holding right-side keys for deletions
//
struct indel_buffer {

    typedef indel_data idata_value_t;
    typedef std::map<indel_key,idata_value_t> idata_t;
    typedef idata_t::iterator iterator;
    typedef idata_t::const_iterator const_iterator;

    indel_buffer(const unsigned max_indel_size)
        : _max_indel_size(max_indel_size)
    {}

    // position iterators based on left-most indel position:
    iterator pos_iter(const pos_t pos) {
        return _idata.lower_bound(indel_key(pos));
    }
    const_iterator pos_iter(const pos_t pos) const {
        return _idata.lower_bound(indel_key(pos));
    }

    // position iterators which return (at least) all indels with a 
    // left or right breakpoint in the range.
    //
    // Note:
    // 1) indels which encompass the range are not returned
    // 2) some non-intersecting indels may be returned in the
    //    iteration range
    //
    std::pair<iterator,iterator>
    pos_range_iter(const pos_t begin_pos, const pos_t end_pos);

    std::pair<const_iterator,const_iterator>
    pos_range_iter(const pos_t begin_pos, const pos_t end_pos) const;

    // return NULL if no indel found:
    indel_data*
    get_indel_data_ptr(const indel_key& ik) {
        const iterator i(_idata.find(ik));
        return ((i==_idata.end()) ? NULL : &(i->second) );
    }

    const indel_data*
    get_indel_data_ptr(const indel_key& ik) const {
        const const_iterator i(_idata.find(ik));
        return ((i==_idata.end()) ? NULL : &(i->second) );
    }

    // returns true if this indel is novel to the buffer
    //
    // is_shared = true is used for indels from other samples where
    // the candidate indel pool is to be synchronized between
    // samples. With this option the  function does add read or contig id's to
    // indel_data.
    //
    bool
    insert_indel(const indel& in,
                 const bool is_shared = false);

    // like insert indel, but used for indels from other samples where
    // the candidate indel pool is to be synchronized between
    // samples. Thus, this function does add read or contig id's to
    // indel_data.
    bool
    insert_shared_indel(const indel& in);

    void
    clear_pos(const pos_t pos);

    // debug dumpers:
    void
    dump_pos(const pos_t pos, std::ostream& os) const;

    void
    dump(std::ostream& os) const;

private:
    unsigned _max_indel_size;
    idata_t _idata; // primary indel data structure based on left-most coordinate of indel range
};



// These functions assume valid iterators:
//
// note these are just stopgaps so that client code can
// voluntarily abstract out the storage mechanism of indel_data,
// ideally we would not give raw iterators to client code, see
// top-level TODO
inline
indel_data&
get_indel_data(const indel_buffer::iterator i) { return (i->second); }

inline
const indel_data&
get_indel_data(const indel_buffer::const_iterator i) { return (i->second); }


#endif