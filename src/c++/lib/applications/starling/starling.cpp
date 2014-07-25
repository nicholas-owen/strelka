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

#include "starling.hh"
#include "starling_info.hh"
#include "starling_run.hh"

#include "blt_common/blt_arg_parse_util.hh"
#include "blt_util/blt_exception.hh"
#include "blt_util/log.hh"
#include "common/Exceptions.hh"
#include "starling_common/starling_arg_parse.hh"
#include "starling_common/starling_option_parser.hh"

#include <cassert>
#include <cstdlib>


namespace
{
const prog_info& pinfo(starling_info::get());
}



void
starling::
runInternal(int argc, char* argv[]) const
{
    starling_options opt;

    // set command-line defaults for starling only:
    opt.gvcf.out_file = "-";
    opt.bsnp_ssd_no_mismatch = 0.35;
    opt.bsnp_ssd_one_mismatch = 0.6;
    opt.min_single_align_score = 20;
    opt.max_win_mismatch = 2;
    opt.max_win_mismatch_flank_size = 20;
    opt.is_min_vexp = true;
    opt.min_vexp = 0.25;


    // TODO double-check with MK:
    ///opt.upstream_oligo_size = 10;

    for (int i(0); i<argc; ++i)
    {
        if (i) opt.cmdline += ' ';
        opt.cmdline += argv[i];
    }

    std::vector<std::string> legacy_starling_args;
    po::variables_map vm;
    try
    {
        po::options_description visible(get_starling_option_parser(opt));
        po::parsed_options parsed(po::command_line_parser(argc,argv).options(visible).allow_unregistered().run());
        po::store(parsed,vm);
        po::notify(vm);

        // allow remaining options to be parsed using old starling command-line parser:
        legacy_starling_args = po::collect_unrecognized(parsed.options,po::include_positional);

    }
    catch (const boost::program_options::error& e)
    {
        pinfo.usage(e.what());
    }

    if ((argc==1) || vm.count("help"))
    {
        pinfo.usage();
    }

    // temp workaround for blt/starling options which are not (yet)
    // under program_options control:
    //
    arg_data ad(legacy_starling_args,pinfo,opt.cmdline);
    legacy_starling_arg_parse(ad,opt);

    finalize_starling_options(pinfo,vm,opt);

    starling_run(opt);
}
