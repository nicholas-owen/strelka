// -*- mode: c++; indent-tabs-mode: nil; -*-
//
// Starka
// Copyright (c) 2009-2014 Illumina, Inc.
//
// This software is provided under the terms and conditions of the
// Illumina Open Source Software License 1.
//
// You should have received a copy of the Illumina Open Source
// Software License 1 along with this program. If not, see
// <https://github.com/sequencing/licenses/>
//

///
/// \author Chris Saunders
///

#pragma once

#include <cassert>

#include <bitset>
#include <iosfwd>


namespace PEDICURE_VCF_FILTERS
{

enum index_t
{
    // SNVs and indels:
    HighDepth,
    WrongCount,
    // snvs only:
    QDS,
    // indels only:
    QDI,
    Repeat,
    iHpol,
    SIZE
};

inline
const char*
get_label(const unsigned idx)
{
    switch (idx)
    {
    case HighDepth:
        return "HighDepth";
    case WrongCount:
        return "WrongCount";
    case QDS:
        return "LowQDS";
    case QDI:
        return "LowQDI";
    case Repeat:
        return "Repeat";
    case iHpol:
        return "iHpol";
    default:
        assert(false && "Unknown vcf filter id");
        return nullptr;
    }
}
}

struct pedicure_shared_modifiers
{
    pedicure_shared_modifiers()
    {
        clear();
    }

    void
    set_filter(const PEDICURE_VCF_FILTERS::index_t i)
    {
        filters.set(i);
    }

    void
    write_filters(std::ostream& os) const;

    void
    clear()
    {
        filters.reset();
    }

    std::bitset<PEDICURE_VCF_FILTERS::SIZE> filters;
};


std::ostream& operator<<(std::ostream& os,const pedicure_shared_modifiers& shmod);