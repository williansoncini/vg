/** \file paths_main.cpp
 *
 * Defines the "vg paths" subcommand, which reads paths in the graph.
 */


#include <omp.h>
#include <unistd.h>
#include <getopt.h>

#include <algorithm>
#include <iostream>

#include "subcommand.hpp"

#include "../vg.hpp"
#include "../xg.hpp"
#include "../gbwt_helper.hpp"
#include "../stream/vpkg.hpp"
#include "../stream/stream.hpp"

using namespace std;
using namespace vg;
using namespace vg::subcommand;

void help_paths(char** argv) {
    cerr << "usage: " << argv[0] << " paths [options]" << endl
         << "options:" << endl
         << "  input:" << endl
         << "    -v, --vg FILE         use the paths in this vg FILE" << endl
         << "    -x, --xg FILE         use the paths in the XG index FILE" << endl
         << "    -g, --gbwt FILE       use the threads in the GBWT index in FILE" << endl
         << "                          (XG index is required for most output options; -g takes priority over -x)" << endl
         << "  output:" << endl
         << "    -X, --extract-gam     return (as GAM alignments) the stored paths in the graph" << endl
         << "    -V, --extract-vg      return (as path-only .vg) the queried paths (requires -x -g and -q or -Q)" << endl
         << "    -L, --list            return (as a list of names, one per line) the path (or thread) names" << endl
         << "    -E, --lengths         return list of path names (as with -L) but paired with their lengths" << endl
         << "  path selection:" << endl
         << "    -Q, --paths-by STR    select the paths with the given name prefix (XG, GBWT)" << endl
         << "    -S, --sample STR      select the threads for this sample (GBWT)" << endl;
}

int main_paths(int argc, char** argv) {

    if (argc == 2) {
        help_paths(argv);
        return 1;
    }

    bool extract_as_gam = false;
    bool extract_as_vg = false;
    bool list_names = false;
    string xg_file;
    string vg_file;
    string gbwt_file;
    string path_prefix;
    string sample_name;
    bool list_lengths = false;
    size_t output_formats = 0, selection_criteria = 0;

    int c;
    optind = 2; // force optind past command positional argument
    while (true) {
        static struct option long_options[] =

        {
            {"vg", required_argument, 0, 'v'},
            {"xg", required_argument, 0, 'x'},
            {"gbwt", required_argument, 0, 'g'},
            {"extract-gam", no_argument, 0, 'X'},
            {"extract-vg", no_argument, 0, 'V'},
            {"list", no_argument, 0, 'L'},
            {"lengths", no_argument, 0, 'E'},
            {"paths-by", required_argument, 0, 'Q'},
            {"sample", required_argument, 0, 'S'},

            // Hidden options for backward compatibility.
            {"threads", no_argument, 0, 'T'},
            {"threads-by", required_argument, 0, 'q'},

            {0, 0, 0, 0}
        };

        int option_index = 0;
        c = getopt_long (argc, argv, "hLXv:x:g:Q:VES:Tq:",
                long_options, &option_index);

        // Detect the end of the options.
        if (c == -1)
            break;

        switch (c)
        {

        case 'v':
            vg_file = optarg;
            break;

        case 'x':
            xg_file = optarg;
            break;

        case 'g':
            gbwt_file = optarg;
            break;
            
        case 'X':
            extract_as_gam = true;
            output_formats++;
            break;

        case 'V':
            extract_as_vg = true;
            output_formats++;
            break;

        case 'L':
            list_names = true;
            output_formats++;
            break;

        case 'E':
            list_names = true;
            list_lengths = true;
            output_formats++;
            break;

        case 'Q':
            path_prefix = optarg;
            selection_criteria++;
            break;

        case 'S':
            sample_name = optarg;
            selection_criteria++;
            break;

        case 'T':
            std::cerr << "warning: [vg paths] option --threads is obsolete and unnecessary" << std::endl;
            break;

        case 'q':
            std::cerr << "warning: [vg paths] option --threads-by is deprecated; please use --paths-by" << std::endl;
            path_prefix = optarg;
            selection_criteria++;
            break;

        case 'h':
        case '?':
            help_paths(argv);
            exit(1);
            break;

        default:
            abort ();
        }
    }

    if (!vg_file.empty() && !(xg_file.empty() && gbwt_file.empty())) {
        std::cerr << "error: [vg paths] cannot read input from multiple sources" << std::endl;
        std::exit(EXIT_FAILURE);
    }
    if (!gbwt_file.empty()) {
        bool need_xg = (extract_as_gam | extract_as_vg | list_lengths);
        if (need_xg && xg_file.empty()) {
            std::cerr << "error: [vg paths] an XG index is needed for extracting threads in -X, -V, or -E format" << std::endl;
            std::exit(EXIT_FAILURE);
        }
        if (!need_xg && !xg_file.empty()) {
            // TODO: This should be an error, but we display a warning instead for backward compatibility.
            //std::cerr << "error: [vg paths] cannot read input from multiple sources" << std::endl;
            //std::exit(EXIT_FAILURE);
            std::cerr << "warning: [vg paths] XG index is unnecessary for listing GBWT threads" << std::endl;
        }
    }
    if (output_formats != 1) {
        std::cerr << "error: [vg paths] one output format (-X, -V, -L, or -E) must be specified" << std::endl;
        std::exit(EXIT_FAILURE);
    }
    if (selection_criteria > 1) {
        std::cerr << "error: [vg paths] multiple selection criteria (-Q, -S) cannot be used" << std::endl;
        std::exit(EXIT_FAILURE);
    }
    if (!sample_name.empty() && gbwt_file.empty()) {
        std::cerr << "error: [vg paths] selection by sample name only works with a GBWT index" << std::endl;
        std::exit(EXIT_FAILURE);
    }
    
    // Load whatever indexes we were given
    unique_ptr<VG> graph;
    if (!vg_file.empty()) {
        // We want a vg
        graph = unique_ptr<VG>(new VG());
        // Load the vg
        get_input_file(vg_file, [&](istream& in) {
            graph->from_istream(in);
        });
    }
    unique_ptr<xg::XG> xg_index;
    if (!xg_file.empty()) {
        // We want an xg
        xg_index = unique_ptr<xg::XG>();
        // Load the xg
        get_input_file(xg_file, [&](istream& in) {
            xg_index = stream::VPKG::load_one<xg::XG>(in);
        });
    }
    unique_ptr<gbwt::GBWT> gbwt_index;
    if (!gbwt_file.empty()) {
        // We want a gbwt
        
        // Load the GBWT from its container
        gbwt_index = stream::VPKG::load_one<gbwt::GBWT>(gbwt_file);

        if (gbwt_index.get() == nullptr) {
          // Complain if we couldn't.
          cerr << "error: [vg paths] unable to load gbwt index file" << endl;
          exit(1);
        }
    }
    
    
    if (gbwt_index.get() != nullptr) {

        if (!(gbwt_index->hasMetadata() && gbwt_index->metadata.hasPathNames())) {
            std::cerr << "error: [vg paths] the GBWT index does not contain thread names" << std::endl;
            std::exit(EXIT_FAILURE);
        }

        // Select the threads we are interested in.
        std::vector<gbwt::size_type> thread_ids;
        if (!sample_name.empty()) {
            if (!gbwt_index->metadata.hasSampleNames()) {
                std::cerr << "error: [vg paths] the GBWT index does not contain sample names" << std::endl;
                std::exit(EXIT_FAILURE);
            }
            gbwt::size_type sample_id = gbwt_index->metadata.sample(sample_name);
            if (sample_id < gbwt_index->metadata.samples()) {
                thread_ids = gbwt_index->metadata.pathsForSample(sample_id);
            }
        } else if(!path_prefix.empty()) {
            for (size_t i = 0; i < gbwt_index->metadata.paths(); i++) {
                std::string name = thread_name(*gbwt_index, i);
                if (name.length() >= path_prefix.length() && std::equal(path_prefix.begin(), path_prefix.end(), name.begin())) {
                    thread_ids.push_back(i);
                }
            }
        } else {
            thread_ids.reserve(gbwt_index->metadata.paths());
            for (size_t i = 0; i < gbwt_index->metadata.paths(); i++) {
                thread_ids.push_back(i);
            }
        }
        
        // We may need to emit a stream of Alignemnts
        unique_ptr<stream::ProtobufEmitter<Alignment>> gam_emitter;
        // Or we might need to emit a stream of VG Graph objects
        unique_ptr<stream::ProtobufEmitter<Graph>> graph_emitter;
        if (extract_as_gam) {
            // Open up a GAM output stream
            gam_emitter = unique_ptr<stream::ProtobufEmitter<Alignment>>(new stream::ProtobufEmitter<Alignment>(cout));
        } else if (extract_as_vg) {
            // Open up a VG Graph chunk output stream
            graph_emitter = unique_ptr<stream::ProtobufEmitter<Graph>>(new stream::ProtobufEmitter<Graph>(cout));
        }

        // Process the threads.
        for (gbwt::size_type id : thread_ids) {
            std::string name = thread_name(*gbwt_index, id);        

            // We are only interested in the name
            if (list_names && !list_lengths) {
                std::cout << name << endl;
                continue;
            }
            
            // Otherwise we need the actual thread data
            gbwt::vector_type sequence = gbwt_index->extract(gbwt::Path::encode(id, false));
            Path path;
            path.set_name(name);
            size_t rank = 1;
            for (auto node : sequence) {
                // Put each node in the constructed path
                Mapping* m = path.add_mapping();
                Position* p = m->mutable_position();
                p->set_node_id(gbwt::Node::id(node));
                p->set_is_reverse(gbwt::Node::is_reverse(node));
                Edit* e = m->add_edit();
                size_t len = xg_index->node_length(p->node_id());
                e->set_to_length(len);
                e->set_from_length(len);
                m->set_rank(rank++);
            }
            if (extract_as_gam) {
                // Write as an Alignment
                gam_emitter->write(xg_index->path_as_alignment(path));
            } else if (extract_as_vg) {
                // Write as a Path in a VG
                Graph g;
                *(g.add_path()) = path;
                graph_emitter->write(std::move(g));
            }
            if (list_lengths) {
                cout << path.name() << "\t" << path_to_length(path) << endl;
            }
        }
    } else if (graph.get() != nullptr) {
        // Handle non-thread queries from vg
        
        auto check_prefix = [&path_prefix](const string& name) {
            // Note: we're using a filter rather than index, so O(#paths in graph). 
            return path_prefix.empty() ||
            std::mismatch(name.begin(), name.end(),
                          path_prefix.begin(), path_prefix.end()).second == path_prefix.end();
        };
        
        if (list_names) {
          graph->paths.for_each([&list_lengths, &check_prefix](const Path& path) {
                  if (check_prefix(path.name())) {
                      cout << path.name();
                      if (list_lengths) {
                          cout << "\t" << path_to_length(path);
                      }
                      cout << endl;
                  }
              });
        } else if (extract_as_gam) {
            vector<Alignment> alns = graph->paths_as_alignments();
            stream::ProtobufEmitter<Alignment> emitter(cout);
            for (auto& aln : alns) {
                if (check_prefix(aln.name())) {
                    emitter.write(std::move(aln));
                }
            }
        } else if (extract_as_vg) {
            cerr << "error: [vg paths] vg extraction is only defined for prefix queries against a XG/GBWT index pair" << endl;
            exit(1);
        }
    } else if (xg_index.get() != nullptr) {
        // Handle non-thread queries from xg
        if (list_names) {
            // We aren't looking for threads, but we are looking for names.
            size_t max_path = xg_index->max_path_rank();
            for (size_t i = 1; i <= max_path; ++i) {
                cout << xg_index->path_name(i);
                if (list_lengths) {
                    cout << "\t" << xg_index->path_length(i);
                }
                cout << endl;
            }
        } else if (!path_prefix.empty()) {
            vector<Path> got = xg_index->paths_by_prefix(path_prefix);
            if (extract_as_gam) {
                stream::ProtobufEmitter<Alignment> emitter(cout);
                for (auto& path : got) {
                    emitter.write(xg_index->path_as_alignment(path));
                }
            } else if (extract_as_vg) {
                for(auto& path : got) {
                    Graph g;
                    *(g.add_path()) = xg_index->path(path.name());
                    vector<Graph> gb = { g };
                    stream::write_buffered(cout, gb, 0);
                }
            }            
        } else if (extract_as_gam) {
            auto alns = xg_index->paths_as_alignments();
            stream::ProtobufEmitter<Alignment> emitter(cout);
            for (auto& aln : alns) {
                emitter.write(std::move(aln));
            }
        }
    }
    
    return 0;

}

// Register subcommand
static Subcommand vg_paths("paths", "traverse paths in the graph", main_paths);

