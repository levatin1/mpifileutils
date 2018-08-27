#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <regex.h>

#include "mpi.h"

#include "mfu.h"
#include "common.h"
#include "pred.h"

/* TODO: change globals to struct */
static int walk_stat = 1;
static int dir_perm = 0;

/* gettimeofday values when command was started */
uint64_t now_secs;
uint64_t now_usecs;

static void print_usage(void)
{
    printf("\n");
    printf("Usage: dfind [options] <path> ...\n");
    printf("\n");
    printf("Options:\n");
    printf("  -i, --input <file>                      - read list from file\n");
    printf("  -o, --output <file>                     - write processed list to file\n");
    printf("  -v, --verbose                           - verbose output\n");
    printf("  -h, --help                              - print usage\n");
    printf("\n");
    fflush(stdout);
    return;
}

/* apply predicate tests and actions to matching items in flist */
static void mfu_flist_pred(mfu_flist flist, pred_item* p)
{
    uint64_t idx;
    uint64_t size = mfu_flist_size(flist);
    for (idx = 0; idx < size; idx++) {
        pred_execute(flist, idx, p);
    }
    return;
}

/* given a source flist and a predicates,
 * return a new list consisting of all matching items */
static mfu_flist mfu_flist_filter_pred(mfu_flist flist, pred_item* p)
{
    /* create a new list to copy matching items */
    mfu_flist list = mfu_flist_subset(flist);

    /* get size of input list */
    uint64_t size = mfu_flist_size(flist);

    /* iterate over each item in input list */
    uint64_t idx;
    for (idx = 0; idx < size; idx++) {
        /* run string of predicates against item */
        int ret = pred_execute(flist, idx, p);
        if (ret == 0) {
            /* copy item into new list if all predicates pass */
            mfu_flist_file_copy(flist, idx, list);
        }
    }

    /* summarize the list */
    mfu_flist_summarize(list);

    return list;
}

/* look up mtimes for specified file,
 * return secs/nsecs in newly allocated stattimes struct,
 * return NULL on error */
static struct stattimes* get_mtimes(const char* file)
{
    mfu_param_path param_path;
    mfu_param_path_set(file, &param_path);
    if (! param_path.path_stat_valid) {
        return NULL;
    }
    struct stattimes* t = (struct stattimes*) MFU_MALLOC(sizeof(struct stattimes));
    mfu_stat_get_mtimes(&param_path.path_stat, &t->secs, &t->nsecs);
    mfu_param_path_free(&param_path);
    return t;
}

static int add_type(char t)
{
    mode_t* type = (mode_t*) MFU_MALLOC(sizeof(mode_t));
    switch (t) {
    case 'b':
        *type = S_IFBLK;
        break;
    case 'c':
        *type = S_IFCHR;
        break;
    case 'd':
        *type = S_IFDIR;
        break;
    case 'f':
        *type = S_IFREG;
        break;
    case 'l':
        *type = S_IFLNK;
        break;
    case 'p':
        *type = S_IFIFO;
        break;
    case 's':
        *type = S_IFSOCK;
        break;
    
    default:
        /* unsupported type character */
        mfu_free(&type);
        return -1;
        break;
    }

    /* add check for this type */
    pred_add(pred_type, (void *)type);
    return 1;
}

int main (int argc, char** argv)
{
    /* initialize MPI */
    MPI_Init(&argc, &argv);
    mfu_init();

    /* get our rank and the size of comm_world */
    int rank, ranks;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &ranks);

    /* capture current time for any time based queries,
     * to get a consistent value, capture and bcast from rank 0 */
    uint64_t times[2];
    if (rank == 0) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        times[0] = (uint64_t) tv.tv_sec;
        times[1] = (uint64_t) tv.tv_usec;
    }
    MPI_Bcast(times, 2, MPI_UINT64_T, 0, MPI_COMM_WORLD);
    now_secs  = times[0];
    now_usecs = times[1];

    int ch;

    char* inputname  = NULL;
    char* outputname = NULL;
    int walk = 0;
    int text = 1;

    static struct option long_options[] = {
        {"input",     1, 0, 'i'},
        {"output",    1, 0, 'o'},
        {"verbose",   0, 0, 'v'},
        {"help",      0, 0, 'h'},

        { "maxdepth", required_argument, NULL, 'd' },

        { "gid",      required_argument, NULL, 'g' },
        { "group",    required_argument, NULL, 'G' },
        { "uid",      required_argument, NULL, 'u' },
        { "user",     required_argument, NULL, 'U' },

        { "size",     required_argument, NULL, 's' },

        { "name",     required_argument, NULL, 'n' },
        { "path",     required_argument, NULL, 'P' },
        { "regex",    required_argument, NULL, 'r' },

        { "amin",     required_argument, NULL, 'a' },
        { "mmin",     required_argument, NULL, 'm' },
        { "cmin",     required_argument, NULL, 'c' },

        { "atime",    required_argument, NULL, 'A' },
        { "mtime",    required_argument, NULL, 'M' },
        { "ctime",    required_argument, NULL, 'C' },

        { "anewer",   required_argument, NULL, 'B' },
        { "newer",    required_argument, NULL, 'N' },
        { "cnewer",   required_argument, NULL, 'D' },

        { "type",     required_argument, NULL, 't' },

        { "print",    no_argument,       NULL, 'p' },
        { "exec",     required_argument, NULL, 'e' },
        { NULL, 0, NULL, 0 },
    };

    options.maxdepth = INT_MAX;
    
    int usage = 0;
    while (1) {
        int c = getopt_long(
                    argc, argv, "i:o:t:",
                    long_options, NULL
                );

        if (c == -1) {
            break;
        }

        int i;
        int space;
        char* buf;
        struct stattimes* t;
        regex_t* r;
        int ret;
    	switch (c) {
    	case 'e':
    	    space = sysconf(_SC_ARG_MAX);
    	    buf = (char *)MFU_MALLOC(space);
    
    	    for (i = optind-1; strcmp(";", argv[i]); i++) {
    	        if (i > argc) {
                    if (rank == 0) {
    	                printf("%s: exec missing terminating ';'\n", argv[0]);
                    }
    	            exit(1);
    	        }
    	        strncat(buf, argv[i], space);
    	        space -= strlen(argv[i]) + 1; /* save room for space or null */
    	        if (space <= 0) {
                    if (rank == 0) {
    	                printf("%s: exec argument list too long.\n", argv[0]);
                    }
    	            mfu_free(&buf);
    	            continue;
    	        }
    	        strcat(buf, " ");
    	        optind++;
    	    }
    	    buf[strlen(buf)] = '\0'; /* clobbers trailing space */
    	    pred_add(pred_exec, buf);
    	    break;
    
    	case 'd':
    	    options.maxdepth = atoi(optarg);
    	    break;

    	case 'g':
            /* TODO: error check argument */
    	    buf = MFU_STRDUP(optarg);
    	    pred_add(pred_gid, (void *)buf);
    	    break;

    	case 'G':
    	    buf = MFU_STRDUP(optarg);
    	    pred_add(pred_group, (void *)buf);
    	    break;

    	case 'u':
            /* TODO: error check argument */
    	    buf = MFU_STRDUP(optarg);
    	    pred_add(pred_uid, (void *)buf);
    	    break;

    	case 'U':
    	    buf = MFU_STRDUP(optarg);
    	    pred_add(pred_user, (void *)buf);
    	    break;

    	case 's':
    	    buf = MFU_STRDUP(optarg);
    	    pred_add(pred_size, (void *)buf);
    	    break;

    	case 'n':
    	    pred_add(pred_name, MFU_STRDUP(optarg));
    	    break;
    	case 'P':
    	    pred_add(pred_path, MFU_STRDUP(optarg));
    	    break;
    	case 'r':
            r = (regex_t*) MFU_MALLOC(sizeof(regex_t));
            ret = regcomp(r, optarg, 0);
            if (ret) {
                MFU_ABORT(-1, "Could not compile regex: `%s' rc=%d\n", optarg, ret);
            }
    	    pred_add(pred_regex, (void*)r);
    	    break;
    
    	case 'a':
    	    buf = MFU_STRDUP(optarg);
    	    pred_add(pred_amin, (void *)buf);
    	    break;
    	case 'm':
    	    buf = MFU_STRDUP(optarg);
    	    pred_add(pred_mmin, (void *)buf);
    	    break;
    	case 'c':
    	    buf = MFU_STRDUP(optarg);
    	    pred_add(pred_cmin, (void *)buf);
    	    break;

    	case 'A':
    	    buf = MFU_STRDUP(optarg);
    	    pred_add(pred_atime, (void *)buf);
    	    break;
    	case 'M':
    	    buf = MFU_STRDUP(optarg);
    	    pred_add(pred_mtime, (void *)buf);
    	    break;
    	case 'C':
    	    buf = MFU_STRDUP(optarg);
    	    pred_add(pred_ctime, (void *)buf);
    	    break;

    	case 'B':
            t = get_mtimes(optarg);
            if (t == NULL) {
                if (rank == 0) {
    	            printf("%s: can't find file %s\n", argv[0], optarg);
                }
    	        exit(1);
    	    }
    	    pred_add(pred_anewer, (void *)t);
    	    break;
    	case 'N':
            t = get_mtimes(optarg);
            if (t == NULL) {
                if (rank == 0) {
    	            printf("%s: can't find file %s\n", argv[0], optarg);
                }
    	        exit(1);
    	    }
    	    pred_add(pred_mnewer, (void *)t);
    	    break;
    	case 'D':
            t = get_mtimes(optarg);
            if (t == NULL) {
                if (rank == 0) {
    	            printf("%s: can't find file %s\n", argv[0], optarg);
                }
    	        exit(1);
    	    }
    	    pred_add(pred_cnewer, (void *)t);
    	    break;
    
    	case 'p':
    	    pred_add(pred_print, NULL);
    	    break;
    
    	case 't':
            ret = add_type(*optarg);
            if (ret != 1) {
                if (rank == 0) {
    	            printf("%s: unsupported file type %s\n", argv[0], optarg);
                }
    	        exit(1);
            }
    	    break;
    
        case 'i':
            inputname = MFU_STRDUP(optarg);
            break;
        case 'o':
            outputname = MFU_STRDUP(optarg);
            break;
        case 'v':
            mfu_debug_level = MFU_LOG_VERBOSE;
            break;
        case 'h':
            usage = 1;
            break;
        case '?':
            usage = 1;
            break;
        default:
            if (rank == 0) {
                printf("?? getopt returned character code 0%o ??\n", c);
            }
    	}
    }
    
    pred_commit();

    /* paths to walk come after the options */
    int numpaths = 0;
    mfu_param_path* paths = NULL;
    if (optind < argc) {
        /* got a path to walk */
        walk = 1;

        /* determine number of paths specified by user */
        numpaths = argc - optind;

        /* allocate space for each path */
        paths = (mfu_param_path*) MFU_MALLOC((size_t)numpaths * sizeof(mfu_param_path));

        /* process each path */
        char** p = &argv[optind];
        mfu_param_path_set_all((uint64_t)numpaths, (const char**)p, paths);
        optind += numpaths;

        /* don't allow user to specify input file with walk */
        if (inputname != NULL) {
            usage = 1;
        }
    }
    else {
        /* if we're not walking, we must be reading,
         * and for that we need a file */
        if (inputname == NULL) {
            usage = 1;
        }
    }
    
    if (usage) {
        if (rank == 0) {
            print_usage();
        }
        mfu_finalize();
        MPI_Finalize();
        return 0;
    }


    /* create an empty file list */
    mfu_flist flist = mfu_flist_new();

    if (walk) {
        /* walk list of input paths */
        mfu_flist_walk_param_paths(numpaths, paths, walk_stat, dir_perm, flist);
    }
    else {
        /* read data from cache file */
        mfu_flist_read_cache(inputname, flist);
    }

    /* apply predicates to each item in list */
    mfu_flist flist2 = mfu_flist_filter_pred(flist, pred_head);

    /* write data to cache file */
    if (outputname != NULL) {
        if (!text) {
            mfu_flist_write_cache(outputname, flist2);
        } else {
            mfu_flist_write_text(outputname, flist2);
        }
    }

    /* free off the filtered list */
    mfu_flist_free(&flist2);

    /* free users, groups, and files objects */
    mfu_flist_free(&flist);

    /* free predicate list */
    pred_free();

    /* free memory allocated for options */
    mfu_free(&outputname);
    mfu_free(&inputname);

    /* free the path parameters */
    mfu_param_path_free_all(numpaths, paths);

    /* free memory allocated to hold params */
    mfu_free(&paths);

    /* shut down MPI */
    mfu_finalize();
    MPI_Finalize();

    return 0;
}
