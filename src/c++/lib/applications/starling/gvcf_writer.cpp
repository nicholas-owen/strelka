// -*- mode: c++; indent-tabs-mode: nil; -*-
//
// Strelka - Small Variant Caller
// Copyright (c) 2009-2016 Illumina, Inc.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//

///
/// \author Chris Saunders
///

#include "gvcf_writer.hh"

#include "gvcf_header.hh"
#include "indel_overlapper.hh"
#include "variant_prefilter_stage.hh"

#include "blt_util/io_util.hh"

#include <iomanip>
#include <iostream>
#include <sstream>

#include "ScoringModelManager.hh"



//#define DEBUG_GVCF


#ifdef DEBUG_GVCF
#include "blt_util/log.hh"
#endif



gvcf_writer::
gvcf_writer(
    const starling_options& opt,
    const starling_deriv_options& dopt,
    const reference_contig_segment& ref,
    const RegionTracker& nocompress_regions,
    const std::string& sampleName,
    std::ostream* osptr,
    const ScoringModelManager& cm)
    : _opt(opt)
    , _report_range(dopt.report_range.begin_pos,dopt.report_range.end_pos)
    , _ref(ref)
    , _osptr(osptr)
    , _chrom(opt.bam_seq_name.c_str())
    , _dopt(dopt.gvcf)
    , _block(_opt.gvcf)
    , _head_pos(dopt.report_range.begin_pos)
    , _gvcf_comp(opt.gvcf,nocompress_regions)
    , _CM(cm)
{
    assert(_report_range.is_begin_pos);
    assert(_report_range.is_end_pos);

    if (! opt.gvcf.is_gvcf_output())
        throw std::invalid_argument("gvcf_writer cannot be constructed with nothing to do.");

    assert(nullptr != _osptr);
    assert((nullptr !=_chrom) && (strlen(_chrom)>0));

    if (! _opt.gvcf.is_skip_header)
    {
        finish_gvcf_header(_opt,_dopt, _dopt.chrom_depth, sampleName, *_osptr, cm);
    }

    variant_prefilter_stage::add_site_modifiers(_empty_site, _empty_site.smod, cm);
}



void gvcf_writer::write_block_site_record()
{
    if (_block.count<=0) return;
    write_site_record(_block);
    _block.reset();
}



void gvcf_writer::filter_site_by_last_indel_overlap(GermlineDiploidSiteCallInfo& si)
{
    if (_last_indel)
    {
        if (si.pos >= _last_indel->end())
        {
            _last_indel.reset(nullptr);
        }
        else
        {
            indel_overlapper::modify_overlapping_site(*_last_indel, si, _CM);
        }
    }
}

// fill in missing sites
void
gvcf_writer::
skip_to_pos(const pos_t target_pos)
{
    // advance through any indel region by adding individual sites
    while (_head_pos<target_pos)
    {
        GermlineDiploidSiteCallInfo si = get_empty_site(_head_pos);

        add_site_internal(si);
        // Don't do compressed ranges if there is an overlapping indel
        // filters are being applied to the overlapping positions
        if (_last_indel) continue;

        if (_gvcf_comp.is_range_compressable(known_pos_range2(si.pos,target_pos)))
        {
            assert(_block.count!=0);
            _block.count += (target_pos-_head_pos);
            _head_pos= target_pos;
        }
    }
}


void gvcf_writer::process(std::unique_ptr<GermlineSiteCallInfo> si)
{
    skip_to_pos(si->pos);

    if (dynamic_cast<GermlineDiploidSiteCallInfo*>(si.get()) != nullptr)
    {
        add_site_internal(*downcast<GermlineDiploidSiteCallInfo>(std::move(si)));
    }
    else
    {
        add_site_internal(*downcast<GermlineContinuousSiteCallInfo>(std::move(si)));
    }

}

void gvcf_writer::process(std::unique_ptr<GermlineIndelCallInfo> ii)
{
    skip_to_pos(ii->pos);

    // flush any non-variant block before starting:
    write_block_site_record();

    if (dynamic_cast<GermlineDiploidIndelCallInfo*>(ii.get()) != nullptr)
    {
        auto ii_digt(downcast<GermlineDiploidIndelCallInfo>(std::move(ii)));

        write_indel_record(*ii_digt);
        _last_indel = std::move(ii_digt);
    }
    else
    {
        write_indel_record(*downcast<GermlineContinuousIndelCallInfo>(std::move(ii)));
    }
}

void gvcf_writer::flush_impl()
{
    skip_to_pos(_report_range.end_pos);
    write_block_site_record();
}


//Add sites to queue for writing to gVCF
void
gvcf_writer::
add_site_internal(
    GermlineDiploidSiteCallInfo& si)
{
    filter_site_by_last_indel_overlap(si);
    if (si.smod.is_phased_region)
    {
        _head_pos=si.pos+si.phased_ref.length();
    }
    else
    {
        _head_pos=si.pos+1;
    }
    // write_site
    queue_site_record(si);
}

void
gvcf_writer::
add_site_internal(
    GermlineContinuousSiteCallInfo& si)
{
    // TODO: phasing
    _head_pos=si.pos+1;
    // write_site
    queue_site_record(si);
}




static
void
get_visible_alt_order(
    const GermlineDiploidSiteCallInfo& si,
    std::vector<uint8_t>& altOrder)
{
    altOrder.clear();

    // list max_gt alts first:
    for (unsigned b(0); b<N_BASE; ++b)
    {
        if (b==si.dgt.ref_gt) continue;
        if (! DIGT::expect2(b,si.smod.max_gt)) continue;
        altOrder.push_back(b);
    }

#if 0
    // include other alts based on known count:
    for (unsigned b(0); b<N_BASE; ++b)
    {
        if (b==si.dgt.ref_gt) continue;
        if (DIGT::expect2(b,si.smod.max_gt)) continue;
        if (si.known_counts[b] > 0) altOrder.push_back(b);
    }
#endif
}



static
void
print_vcf_alt(
    const std::vector<uint8_t>& altOrder,
    std::ostream& os)
{
    bool is_print(false);
    for (const auto& b : altOrder)
    {
        if (is_print) os << ',';
        os << id_to_base(b);
        is_print=true;
    }
    if (! is_print) os << '.';
}



static
void
print_site_ad(
    const GermlineSiteCallInfo& si,
    const std::vector<uint8_t>& altOrder,
    std::ostream& os)
{
    os << si.alleleObservationCounts(base_to_id(si.ref));

    for (const auto& b : altOrder)
    {
        os << ',' << si.alleleObservationCounts(b);
    }
}



static
void
print_site_ad_strand(
    const GermlineSiteCallInfo& si,
    const std::vector<uint8_t>& altOrder,
    const bool is_fwd_strand,
    std::ostream& os)
{
    os << si.alleleObservationCountsByStrand(is_fwd_strand, base_to_id(si.ref));

    for (const auto& b : altOrder)
    {
        os << ',' << si.alleleObservationCountsByStrand(is_fwd_strand,b);
    }
}



//writes out a SNP or block record
void
gvcf_writer::
write_site_record(
    const GermlineDiploidSiteCallInfo& si) const
{
    std::ostream& os(*_osptr);

    os << _chrom << '\t'  // CHROM
       << (si.pos+1) << '\t'  // POS
       << ".\t";           // ID

    if (si.smod.is_phased_region)
    {
        os  << si.phased_ref << '\t'; // REF
    }
    else
    {
        os  << si.ref << '\t'; // REF
    }

    // ALT
    std::vector<uint8_t> altOrder;
    const bool isNoAlt(si.smod.is_unknown);
    if (isNoAlt)
    {
        os << '.';
    }
    else if (si.smod.is_phased_region)
    {
        os << si.phased_alt;
    }
    else
    {
        get_visible_alt_order(si,altOrder);
        print_vcf_alt(altOrder,os);
    }
    os << '\t';

    // QUAL:
    if (si.is_qual())
    {
        os << si.dgt.genome.snp_qphred;
    }
    else
    {
        os << '.';
    }
    os << '\t';

    // FILTER:
    si.smod.write_filters(os);
    os << '\t';

    // INFO:
    if (si.dgt.is_snp)
    {
        os << "SNVSB=";
        {
            const StreamScoper ss(os);
            os << std::fixed << std::setprecision(1) << si.smod.strand_bias;
        }
        os << ';';
        os << "SNVHPOL=" << si.hpol;
        if (_opt.is_compute_hapscore)
        {
            os << ';';
            os << "HaplotypeScore=" << si.hapscore;
        }

        if (_opt.isReportEVSFeatures)
        {
#ifdef SUPPORT_LEGACY_EVS_TRAINING_SCRIPTS
            os << ';';
            os << "MQ=" << si.mapqRMS;
            os << ';';
            os << "MQ0=" << si.mapqZeroCount;
            os << ';';
            os << "MQRankSum=" << si.MQRankSum;
            os << ';';
            os << "BaseQRankSum=" << si.BaseQRankSum;
            os << ';';
            os << "ReadPosRankSum=" << si.ReadPosRankSum;
            os << ';';
            os << "AvgBaseQ=" << si.avgBaseQ;
            os << ';';
            os << "AvgPos=" << si.rawPos;
            // if you uncomment the following, make sure you also uncomment the matching INFO header entry in gvcf_header.cpp
            //                os << ';';
            //                os << "MapQ0Count=" << si.mapq_zero;

            // N.B. DP is in FORMAT already, and that seems to be where Nondas's code expects to find it, so suppress it here:
            //                os << ';';
            //                os << "DP=" << (si.n_used_calls+si.n_unused_calls);
#endif

            // EVS features may not be computed for certain records, so check first:
            if (! si.smod.features.empty())
            {
                const StreamScoper ss(os);
                os << std::setprecision(5);
                os << ";EVSF=";
                for (unsigned featureIndex(0); featureIndex < GERMLINE_SNV_SCORING_FEATURES::SIZE; ++featureIndex)
                {
                    if (featureIndex > 0)
                    {
                        os << ",";
                    }
                    os << si.smod.features.get(static_cast<GERMLINE_SNV_SCORING_FEATURES::index_t>(featureIndex));
                }
                for (unsigned featureIndex(0); featureIndex < GERMLINE_SNV_SCORING_DEVELOPMENT_FEATURES::SIZE; ++featureIndex)
                {
                    os << ",";
                    os << si.smod.developmentFeatures.get(static_cast<GERMLINE_SNV_SCORING_DEVELOPMENT_FEATURES::index_t>(featureIndex));
                }
            }
        }

        if (si.smod.is_phasing_insufficient_depth)
        {
            os << ";Unphased";
        }
    }
    else
    {
        os << '.';
    }
    os << '\t';

    const bool is_nonref_gt(si.smod.max_gt != si.dgt.ref_gt);
    const bool is_print_pl(is_nonref_gt || si.dgt.is_snp);

    //FORMAT
    os << "GT";
    if (si.dgt.is_snp)
    {
        os << ":GQ";
    }
    os << ":GQX:DP:DPF";
    if (! isNoAlt)
    {
        os << ":AD:ADF:ADR";
    }
    if (is_print_pl)
    {
        os << ":PL";
    }
    os << '\t';

    //SAMPLE
    os << si.get_gt() << ':';
    if (si.dgt.is_snp)
    {
        os << si.smod.gq << ':';
    }
    if (si.smod.is_gqx())
    {
        if (si.smod.empiricalVariantScore>=0)
        {
            os << si.smod.empiricalVariantScore;
        }
        else
        {
            os << si.smod.gqx;
        }
    }
    else
    {
        os << '.';
    }
    os << ':';
    //print DP:DPF
    os << si.n_used_calls << ':'
       << si.n_unused_calls;

    if (isNoAlt)
    {
        // pass
    }
    else if (si.smod.is_phased_region)
    {
        os << ':' << si.phased_AD
           << ':' << si.phased_ADF
           << ':' << si.phased_ADR;
    }
    else
    {
        os << ':';
        print_site_ad(si, altOrder, os);
        os << ':';
        print_site_ad_strand(si, altOrder, true, os);
        os << ':';
        print_site_ad_strand(si, altOrder, false, os);
    }

    if (is_print_pl)
    {
        // print PL values
        os << ':';
        if (si.is_hetalt())
        {
            const unsigned print_gt(si.smod.max_gt);
            const uint8_t a0(DIGT::get_allele(print_gt,0));
            const uint8_t a1(DIGT::get_allele(print_gt,1));
            os << si.dgt.phredLoghood[si.dgt.ref_gt] << ','
               << si.dgt.phredLoghood[DIGT::get_gt_with_alleles(si.dgt.ref_gt,a0)] << ','
               << si.dgt.phredLoghood[DIGT::get_gt_with_alleles(a0,a0)] << ','
               << si.dgt.phredLoghood[DIGT::get_gt_with_alleles(si.dgt.ref_gt,a1)] << ','
               << si.dgt.phredLoghood[DIGT::get_gt_with_alleles(a0,a1)] << ','
               << si.dgt.phredLoghood[DIGT::get_gt_with_alleles(a1,a1)];
        }
        else if (si.dgt.is_haploid() || (si.smod.modified_gt == MODIFIED_SITE_GT::ONE))
        {
            os << si.dgt.phredLoghood[si.dgt.ref_gt] << ','
               << si.dgt.phredLoghood[si.smod.max_gt];
        }
        else
        {
            const unsigned print_gt(si.smod.max_gt);
            const uint8_t a0(DIGT::get_allele(print_gt,0));
            const uint8_t a1(DIGT::get_allele(print_gt,1));
            uint8_t alt(a0);
            if (si.dgt.ref_gt == a0)
            {
                alt = a1;
            }
            os << si.dgt.phredLoghood[si.dgt.ref_gt] << ','
               << si.dgt.phredLoghood[DIGT::get_gt_with_alleles(si.dgt.ref_gt,alt)] << ','
               << si.dgt.phredLoghood[DIGT::get_gt_with_alleles(alt,alt)];
        }
    }

    os << '\n';
}



void
gvcf_writer::
write_site_record(
    const GermlineContinuousSiteCallInfo& si) const
{
    bool site_is_nonref = si.is_nonref();
    auto ref_base_id = base_to_id(si.ref);

    for (auto& call : si.calls)
    {
        std::vector<uint8_t> altOrder;
        const bool is_no_alt(call._base == ref_base_id);
        if (! is_no_alt)
        {
            altOrder.push_back(call._base);
        }

        // do not output the call for reference if the site has variants unless it is forced output
        if (!si.forcedOutput && site_is_nonref && is_no_alt)
            continue;

        std::ostream& os(*_osptr);

        os << _chrom << '\t'  // CHROM
           << (si.pos+1) << '\t'  // POS
           << ".\t";           // ID

        os  << si.ref << '\t'; // REF

        std::string gt(si.get_gt(call));

        // ALT
        if (is_no_alt)
            os << ".";
        else
            os << id_to_base(call._base);
        os << '\t';

        // QUAL: TODO - need separate calc?
        os << call.gq << '\t';

        // FILTER:
        call.write_filters(os);
        os << '\t';

        // INFO
        std::ostringstream info;

        if (si._is_snp)
        {
            info << "SNVSB=";
            {
                const StreamScoper ss(info);
                info << std::fixed << std::setprecision(1) << call.strand_bias;
            }
            info << ';';
            info << "SNVHPOL=" << si.hpol;
        }

        if (!is_no_alt)
        {
            if (_opt.do_codon_phasing)
            {
                if (!info.str().empty())
                    info << ";";
                info << "Unphased"; // TODO: placeholder until we do phasing on continuous variants
            }
        }

        os << (info.str().empty() ? "." : info.str()) << "\t";

        //FORMAT
        os << "GT";
        os << ":GQ";
        os << ":GQX";
        os << ":DP:DPF";
        if (!is_no_alt)
        {
            os << ":AD:ADF:ADR";
        }
        os << ":VF";

        os << '\t';

        //SAMPLE
        os << gt
           << ':' << call.gq
           << ':' << call.gqx;

        // DP:DPF
        os << ':' << si.n_used_calls << ':' << si.n_unused_calls;

        if (!is_no_alt)
        {
            os << ':';
            print_site_ad(si, altOrder, os);
            os << ':';
            print_site_ad_strand(si, altOrder, true, os);
            os << ':';
            print_site_ad_strand(si, altOrder, false, os);
        }

        {
            const StreamScoper ss(os);
            os << ':' << std::fixed << std::setprecision(3) << call.variant_frequency();
        }
        os << '\n';
    }
}



void
gvcf_writer::
write_site_record(
    const gvcf_block_site_record& si) const
{
    std::ostream& os(*_osptr);

    os << _chrom << '\t'  // CHROM
       << (si.pos+1) << '\t'  // POS
       << ".\t";           // ID

    os  << si.ref << '\t'; // REF

    // ALT
    os << '.';
    os << '\t';

    // QUAL:
    os << '.';
    os << '\t';

    // FILTER:
    si.write_filters(os);
    os << '\t';

    // INFO:
    if (si.count>1)
    {
        os << "END=" << (si.pos+si.count) << ';';
        os << _dopt.block_label;
    }
    else
    {
        os << '.';
    }
    os << '\t';

    //FORMAT
    os << "GT";
    os << ":GQX:DP:DPF";
    os << '\t';

    //SAMPLE
    os << si.get_gt() << ':';
    if (si.has_call)
    {
        os << _block.block_gqx.min();
    }
    else
    {
        os << '.';
    }
    os << ':';
    //print DP:DPF
    os << _block.block_dpu.min() << ':'
       << _block.block_dpf.min();
    os << '\n';
}



void
gvcf_writer::
write_indel_record(
    const GermlineDiploidIndelCallInfo& ii) const
{
    std::ostream& os(*_osptr);
    auto& call(ii.first());

    os << _chrom << '\t'   // CHROM
       << ii.pos << '\t'   // POS
       << ".\t"            // ID
       << call._indelReportInfo.vcf_ref_seq << '\t'; // REF

    // ALT

    for (unsigned i = 0; i <ii._calls.size(); ++i)
    {
        if (i > 0) os << ',';
        os << ii._calls[i]._indelReportInfo.vcf_indel_seq;
    }
    os << '\t';

    os << call._dindel.indel_qphred << '\t'; //QUAL

    // FILTER:
    call.write_filters(os);
    os << '\t';

    // INFO
    os << "CIGAR=";
    for (unsigned i=0; i < ii._calls.size(); ++i)
    {
        if (i > 0) os << ',';
        os << ii._calls[i].cigar;
    }
    os << ';';
    os << "RU=";
    for (unsigned i = 0; i <ii._calls.size(); ++i)
    {
        const auto& iri(ii._calls[i]._indelReportInfo);
        if (i > 0) os << ',';
        if (iri.is_repeat_unit() && iri.repeat_unit.size() <= 20)
        {
            os << iri.repeat_unit;
        }
        else
        {
            os << '.';
        }
    }
    os << ';';
    os << "REFREP=";
    for (unsigned i = 0; i <ii._calls.size(); ++i)
    {
        const auto& iri(ii._calls[i]._indelReportInfo);
        if (i > 0) os << ',';

        if (iri.is_repeat_unit())
        {
            os << iri.ref_repeat_count;
        }
        else
        {
            os << '.';
        }
    }
    os << ';';
    os << "IDREP=";
    for (unsigned i = 0; i <ii._calls.size(); ++i)
    {
        const auto& iri(ii._calls[i]._indelReportInfo);

        if (i > 0) os << ',';

        if (iri.is_repeat_unit())
        {
            os << iri.indel_repeat_count;
        }
        else
        {
            os << '.';
        }
    }

    if (_opt.isReportEVSFeatures)
    {
        // EVS features may not be computed for certain records, so check first:
        if (! call.features.empty())
        {
            const StreamScoper ss(os);
            os << std::setprecision(5);
            os << ";EVSF=";
            for (unsigned featureIndex(0); featureIndex < GERMLINE_INDEL_SCORING_FEATURES::SIZE; ++featureIndex)
            {
                if (featureIndex > 0)
                {
                    os << ",";
                }
                os << call.features.get(static_cast<GERMLINE_INDEL_SCORING_FEATURES::index_t>(featureIndex));
            }
            for (unsigned featureIndex(0); featureIndex < GERMLINE_INDEL_SCORING_DEVELOPMENT_FEATURES::SIZE; ++featureIndex)
            {
                os << ",";
                os << call.developmentFeatures.get(static_cast<GERMLINE_INDEL_SCORING_DEVELOPMENT_FEATURES::index_t>(featureIndex));
            }
        }
    }

    os << '\t';

    //FORMAT
    os << "GT:GQ:GQX:DPI:AD:ADF:ADR:PL" << '\t';

    //SAMPLE
    os << ii.get_gt() << ':'
       << call.gq;

    os << ':' << ((call.empiricalVariantScore>=0) ? call.empiricalVariantScore : call.gqx);

    os << ':' << call._indelSampleReportInfo.tier1Depth;

    // SAMPLE AD/ADF/ADR:
    {
        auto orderRefReads = [](const GermlineDiploidIndelSimpleGenotypeInfo& a, const GermlineDiploidIndelSimpleGenotypeInfo& b)
        {
            return (a._indelSampleReportInfo.n_q30_ref_reads < b._indelSampleReportInfo.n_q30_ref_reads);
        };

        const auto maxRefCountIter(
            std::max_element(ii._calls.begin(),ii._calls.end(),orderRefReads));

        const auto& maxRefIsri(maxRefCountIter->_indelSampleReportInfo);

        // AD
        os << ':' << maxRefIsri.n_q30_ref_reads;
        for (const auto& icall : ii._calls)
        {
            os << ',' << icall._indelSampleReportInfo.n_q30_indel_reads;
        }

        // ADF
        os << ':' << maxRefIsri.n_q30_ref_reads_fwd;
        for (const auto& icall : ii._calls)
        {
            os << ',' << icall._indelSampleReportInfo.n_q30_indel_reads_fwd;
        }

        // ADR
        os << ':' << maxRefIsri.n_q30_ref_reads_rev;
        for (const auto& icall : ii._calls)
        {
            os << ',' << icall._indelSampleReportInfo.n_q30_indel_reads_rev;
        }
    }

    // PL field
    os << ":";
    const unsigned icount(ii._calls.size());
    if (icount == 1)
    {
        using namespace STAR_DIINDEL;
        const auto& dindel(ii._calls[0]._dindel);
        const auto& pls(dindel.phredLoghood);
        if (dindel.is_haploid())
        {
            os << pls[NOINDEL] << ','
               << pls[HOM];
        }
        else
        {
            os << pls[NOINDEL] << ','
               << pls[HET] << ','
               << pls[HOM];
        }
    }
    else if (icount == 2)
    {
        // very roughly approximate the overlapping indel PL values
        //
        // 1. 0/0 - this is always maxQ
        // 2. 0/1 - set ot 0/0 from indel1
        // 3. 1/1 - set to 1/1 from indel0
        // 4. 0/2 - set to 0/0 from indel0
        // 5. 1/2 - this is always 0
        // 6. 2/2 - set to 1/1 from indel1
        //
        using namespace STAR_DIINDEL;
        const auto& pls0(ii._calls[0]._dindel.phredLoghood);
        const auto& pls1(ii._calls[1]._dindel.phredLoghood);

        os << GermlineDiploidIndelSimpleGenotypeInfoCore::maxQ << ','
           << pls1[NOINDEL] << ','
           << pls0[HOM] << ','
           << pls0[NOINDEL] << ','
           << 0 << ','
           << pls1[HOM];
    }
    else
    {
        assert(false && "Unexpected indel count");
    }

    os << '\n';
}



void
gvcf_writer::
write_indel_record(
    const GermlineContinuousIndelCallInfo& ii) const
{
    std::ostream& os(*_osptr);

    for (auto& call : ii.calls)
    {
        os << _chrom << '\t'   // CHROM
           << ii.pos << '\t'   // POS
           << ".\t"            // ID
           << call._indelReportInfo.vcf_ref_seq << '\t'; // REF

        // ALT
        os << call._indelReportInfo.vcf_indel_seq;
        os << '\t';

        os << call.gq << '\t'; //QUAL

        // FILTER:
        call.write_filters(os);
        os << '\t';

        // INFO
        os << "CIGAR=";
        os << call.cigar;
        os << ';';
        os << "RU=";
        if (call._indelReportInfo.is_repeat_unit() && call._indelReportInfo.repeat_unit.size() <= 20)
        {
            os << call._indelReportInfo.repeat_unit;
        }
        else
        {
            os << '.';
        }
        os << ';';
        os << "REFREP=";
        if (call._indelReportInfo.is_repeat_unit())
        {
            os << call._indelReportInfo.ref_repeat_count;
        }

        os << ';';
        os << "IDREP=";
        if (call._indelReportInfo.is_repeat_unit())
        {
            os << call._indelReportInfo.indel_repeat_count;
        }


        os << '\t';

        //FORMAT
        os << "GT:GQ:GQX:DPI:AD:ADF:ADR:VF" << '\t';

        //SAMPLE
        os << ii.get_gt() << ':'
           << call.gq;

        os << ':' << call.gqx;

        os << ':' << call._indelSampleReportInfo.tier1Depth;

        // AD:
        os << ':' << call._indelSampleReportInfo.n_q30_ref_reads
           << ',' << call._indelSampleReportInfo.n_q30_indel_reads;

        // ADF
        os << ':' << call._indelSampleReportInfo.n_q30_ref_reads_fwd
           << ',' << call._indelSampleReportInfo.n_q30_indel_reads_fwd;

        // ADR
        os << ':' << call._indelSampleReportInfo.n_q30_ref_reads_rev
           << ',' << call._indelSampleReportInfo.n_q30_indel_reads_rev;

        // VF
        {
            const StreamScoper ss(os);
            os << ':' << std::setprecision(3) << call.variant_frequency();
        }
        os << '\n';
    }
}
