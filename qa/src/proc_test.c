/*
 * Copyright (c) 1997-2001 Silicon Graphics, Inc.  All Rights Reserved.
 */

/*
 * Test Plan
 * ---------
 *
 * 0. Agent has associated PMNS, testable say thru PMNS functions.
 *
 * Test the agent exported functions using the PMAPI:
 * (We could call the agent functions directly but we might as well test
 *  that the whole package is working - i.e. communication via pmcd).
 *
 * 1. proc_desc     - pmGetDesc
 * 2. proc_text     - pmLookupText, pmLookupInDomText
 * 3. proc_instance - pmGetInDom, pmNameInDom, pmLookupInDom
 * 4. proc_profile  - pmAddProfile, pmDelProfile
 * 5. proc_fetch    - pmFetch
 * 6. proc_store    - pmStore
 *
 * Not used: proc_control
 *
 *
 * Method
 * ------
 *
 * 1. desc
 * a. Test that all specified metrics can have their desc retrieved without error 
 * b. Test that all spec. metrics have the same indom.
 * Note: ideally we would want to make sure that all the desc values returned are
 *       correct but that means basically just duplicating the agent's code.
 *
 * 3. instance
 * a. Output the instance map, name-->inst, inst-->name
 *    This output can be tested by a script to look for certain
 *    processes.
 * b. Verify that names and ids are consistent with each other.
 *
 * 4.5. profile/fetch
 * a. Profile is set correctly - look at context dump
 * b. Fetch on profile and ensure that the results only have the specified
 *    instances.
 * Note: ideally we would to ensure that the values returned are correct.
 *	 This, however, would be a tricky thing for a lot of metrics.
 *	 The metrics are continually changing for the processes.
 *
 * 6. store
 * a. Ensure it fails appropriately.
 *
 * TODO
 * ----
 * 0, 2,
 * 
 */

#include <ctype.h>
#include <sys/wait.h>
#include <pcp/pmapi.h>
#include "libpcp.h"

#define MAXMETRICS 1024

int	verbose = 0;
char	*host = "localhost";
char	*pmnsfile = PM_NS_DEFAULT;
int	nmetrics;
const char *metrics[MAXMETRICS];
pmID	pmids[MAXMETRICS];
pmDesc	descs[MAXMETRICS];
pmInDom	indom;
int	iterations = 1;
int	all_n;
int	*all_inst;
char	**all_names;
pid_t   child_pid;
int	is_hotproc = 0;
int	refresh = 1;

/* format of an entry in /proc */
char proc_fmt[8];   /* export for procfs fname conversions */


/*
 * getargs
 */

void
getargs(int argc, char **argv)
{
    static char	*usage = " [-h hostname] [-D debugspec] [-n pmnsfile] "
			 "[-i iterations] [-t refresh] [-v] "
			 "metric [metric ...]";
    int		errflag = 0;
    char	*endnum;
    int		c;
    int		i;
    int		sts;

    pmSetProgname(argv[0]);

    while ((c = getopt(argc, argv, "D:h:n:i:t:v")) != EOF) {
	switch (c) {

	case 'D':	/* debug options */
	    sts = pmSetDebug(optarg);
	    if (sts < 0) {
		fprintf(stderr, "%s: unrecognized debug options specification (%s)\n",
		    pmGetProgname(), optarg);
		errflag++;
	    }
	    break;

	case 'h':	/* hostname for PMCD to contact */
	    host = optarg;
	    break;
	
	case 'i':	/* iterations */
	    iterations = (int)strtol(optarg, &endnum, 10);
	    if (*endnum != '\0') {
		fprintf(stderr, "%s: -i requires numeric argument\n", pmGetProgname());
		errflag++;
	    }
	    break;

	case 'n':	/* alternative name space file */
	    pmnsfile = optarg;
	    break;

	case 't':
	    refresh = (int)strtol(optarg, &endnum, 10);
	    if (*endnum != '\0') {
		fprintf(stderr, "%s: -t requires numeric argument\n", pmGetProgname());
		errflag++;
	    }
	    break;

	case 'v':
	    verbose = 1;
	    break;

	case '?':
	default:
	    errflag++;
	    break;
	}
    }

    if (errflag) {
USAGE:
	fprintf(stderr, "Usage: %s %s\n", pmGetProgname(), usage);
	exit(1);
    }

    /* non-flag args are argv[optind] ... argv[argc-1] */
    if (optind >= argc)
	goto USAGE;

    /* note metrics and dump them out */
    for (i = 0; i < argc - optind; i++) {
	metrics[i] = argv[optind+i];
	if (strncmp(metrics[i], "hotproc.", 8) == 0) {
	   if (i > 0 && !is_hotproc) {
		printf("%s: Error: all metrics should be from same agent\n",
		       pmGetProgname()); 
		exit(1);
           }
	   is_hotproc = 1; 
	}
	else if (strncmp(metrics[i], "proc.", 5) == 0) {
	   if (i > 0 && is_hotproc) {
		printf("%s: Error: all metrics should be from same agent\n",
		       pmGetProgname()); 
		exit(1);
           }
	   is_hotproc = 0; 
	}
	else {
	    printf("%s: Error: all metrics should be from "
		   "proc or hotproc agent: %s\n", pmGetProgname(), metrics[i]);
	    exit(1);
	}
	printf("metrics[%d] = <%s>\n", i, metrics[i]);
    }
    nmetrics = i;

    if (nmetrics <= 0)
	goto USAGE;
}


void
print_banner_start(char *msg)
{
   int len = strlen(msg);
   int i;

   printf("\n");
   for (i = 0; i < len+14; i++) printf("=");
   printf("\n");
   printf("=== Test: %s ===\n", msg);
   for (i = 0; i < len+14; i++) printf("=");
   printf("\n");

}

void
print_banner_end(char *msg)
{
   int len = strlen(msg);
   int i;

   printf("\n");
   for (i = 0; i < len+21; i++) printf("=");
   printf("\n");
   printf("=== End Of Test: %s ===\n", msg);
   for (i = 0; i < len+21; i++) printf("=");
   printf("\n");

}

static void
set_proc_fmt(void)
{
    DIR *procdir;
    const char *procfs = "/proc";
    struct dirent *directp;
    int		ndigit;
    int		proc_entry_len; /* number of chars in procfs entry */

    if (access(procfs, R_OK) != 0) {
	fprintf(stderr, "No /proc pseudo filesystem on this platform\n");
	exit(1);
    }

    if ((procdir = opendir(procfs)) == NULL) {
	perror(procfs);
	return;
    }
    proc_entry_len = -1;
    for (rewinddir(procdir); (directp = readdir(procdir));) {
	if (!isdigit((int)directp->d_name[0]))
	    continue;
	ndigit = (int)strlen(directp->d_name);
	if (proc_entry_len == -1) {
	    proc_entry_len = ndigit;
	    pmsprintf(proc_fmt, sizeof(proc_fmt), "%%0%dd", proc_entry_len);
	}
	else if (ndigit != proc_entry_len) {
	    /*
	     * different lengths, so not fixed width ... this is the
	     * Linux way
	     */
	    pmsprintf(proc_fmt, sizeof(proc_fmt), "%%d");
	    break;
	}
    }
    closedir(procdir);
}

/* 
 * 0.
 * Does NOT really verify PMNS 
 * get pmids for rest of pm functions
 */
void
test_PMNS(void)
{
    int sts;
    int i;

    print_banner_start("PMNS");
    if ((sts = pmLookupName(nmetrics, metrics, pmids)) < 0) {
	printf("pmLookupName: %s\n", pmErrStr(sts));
	for (i = 0; i < nmetrics; i++) {
	    if (pmids[i] == PM_ID_NULL)
		printf("	%s - not known\n", metrics[i]);
	}
	exit(1);
    }
    print_banner_end("PMNS");

}

/*
 * 1. check all the descriptors
 */
void
test_desc(void)
{
    int i, sts;

    print_banner_start("desc");

    /* lookup all descriptors and ensure matching non-null indom */
    if ((sts = pmLookupDescs(nmetrics, pmids, descs)) < 0) {
	fprintf(stderr, "pmLookupDescs: %s\n", pmErrStr(sts));
	exit(1);
    }
    if ((indom = descs[0].indom) == PM_INDOM_NULL) {
	fprintf(stderr, "pmLookupDescs: NULL indom for initial metric\n");
	fprintf(stderr, "Associated metric = %s (%s)\n", metrics[0], pmIDStr(pmids[0]));
	exit(1);
    }
    for (i=1; i < nmetrics; i++) {
	if (descs[i].indom != indom) {
	    fprintf(stderr, "metric <%s> has indom = %d, different to metric <%s> indom = %d\n",
	    metrics[i], descs[i].indom, metrics[0], indom);
	    fprintf(stderr, "This test requires all metrics have the same indom\n");
	    exit(1);
	}
    }

    print_banner_end("desc");

}


/*
 * 3.
 * Test out proc_instance
 * Using pmGetInDom(), pmNameInDom(), pmLookupInDom()
 * Now get metrics for the entire instance domain
 *
 * Does some sanity checks on names and ids
 */

void
test_instance(void)
{
    int		sts;
    int		i;

    print_banner_start("instance");
    if (indom == PM_INDOM_NULL)
	return;

    fflush(stdout);
    if ((child_pid=fork()) == 0) {
	/* child sleeps and then exits */
	sleep(2*refresh+1);
	exit(0);
    }
    printf("cpid=%" FMT_PID "\n", child_pid);
 
    if (is_hotproc) {
	/* sleep so that hotprocs can update active list */
        sleep(2*refresh);
    }
    printf("\n--- GetInDom ---\n");
    sts = pmGetInDom(indom, &all_inst, &all_names);
    if (sts < 0) {
	printf("%s: pmGetInDom: %s\n", pmGetProgname(), pmErrStr(sts));
	exit(1);
    }
    else
	all_n = sts;

    /* print the instance id's (i.e. pids) match the first field in the name */
    for (i=0; i < all_n; i++) {
	int	inst;
        if (verbose)
	  printf("  instance map [%d \"%s\"]\n", all_inst[i], all_names[i]);

	/* e.g. inst=0, name="00000 sched" */
	if (sscanf(all_names[i], proc_fmt, &inst) != 1) {
	    printf("%s: Error: cannot get PID from instname\n", pmGetProgname());
	    printf("%s: <id,name> = <%ld,\"%s\">\n", pmGetProgname(),
		   (long)all_inst[i], all_names[i]);
	    exit(1);
	}
	if (inst != all_inst[i]) {
	    printf("%s: Error: instname is wrong\n", pmGetProgname());
	    printf("%s: <id,name> = <%ld,\"%s\"> != %d (fmt=%s)\n", pmGetProgname(),
		   (long)all_inst[i], all_names[i], inst, proc_fmt);
	    exit(1);
	}
    }

    /* parent waits for child to exit */
    /* so that the the following lookups will NOT be able to find it */
    wait(&sts);


    printf("\n--- LookupInDom ---\n");
    for (i = 0; i < all_n; i++) {
	int inst, x;
	sts = pmLookupInDom(indom, all_names[i]);
	if (sts < 0) {
	    if (sts == PM_ERR_INST) {
		if (all_inst[i] == child_pid)
		    printf("  Death of child detected, pid=%" FMT_PID "\n", child_pid); 
		/* ignore deaths */
		continue;
	    }
	    else {
		printf("%s: pmLookupInDom: %s\n", pmGetProgname(), pmErrStr(sts));
		exit(1);
	    }
	}
	inst = sts;
	if (verbose)
	    printf("  instance lookup \"%s\" --> %d\n", all_names[i], inst);  
	sscanf(all_names[i], proc_fmt, &x);
	if (x != inst) {
	    printf("%s: Error: inst is wrong, expect: %d got: %d\n",
		pmGetProgname(), inst, x);
	    printf("%s: Expected=%d, Actual=%d\n", pmGetProgname(), x, inst);
	    exit(1);
	}
    }/*for*/

    printf("\n--- NameInDom ---\n");
    for (i = 0; i < all_n; i++) {
	char *name;
        char x[100];

	sts = pmNameInDom(indom, all_inst[i], &name);
	if (sts < 0) {
	    if (sts == PM_ERR_INST) {
		if (all_inst[i] == child_pid)
		    printf("  Death of child detected\n"); 
		/* ignore deaths */
		continue;
	    }
	    else {
		printf("%s: pmNameInDom: %s\n", pmGetProgname(), pmErrStr(sts));
		exit(1);
	    }
	}
	if (verbose)
	    printf("  instance name %d --> \"%s\"\n", all_inst[i], name);  
	pmsprintf(x, sizeof(x), proc_fmt, all_inst[i]);
	if (strncmp(name, x, strlen(x)) != 0 ||
	    (name[strlen(x)] != '\0' && name[strlen(x)] != ' ')) {
	    /* try w/out leading zeroes */
	    char	*q;
	    pmsprintf(x, sizeof(x), "%d", all_inst[i]);
	    for (q = name; *q && *q == '0'; q++)
		;
	    if (strncmp(q, x, strlen(x)) != 0 ||
		(q[strlen(x)] != '\0' && q[strlen(x)] != ' ')) {
		printf("%s: Error: name is wrong\n", pmGetProgname());
		printf("%s: Expected=\"%s\", Actual=\"%s\"\n", pmGetProgname(), x, name);
		exit(1);
	    }
	}
	free(name);
    }/*for*/

    print_banner_end("instance");
}


/*
 * Tests 4. and 5.
 * Set up an explicit profile of ourself and our parent
 * If any of the metrics are not in the proc indom, we'll get no values back.
 * Checks if profile is being handled correctly.
 * Checks if fetch is using profile correctly.
 */
void
test_prof_fetch(void)
{
    int 	sts;
    int		i;
    int		pids[2];
    pmResult	*result1, *result2;

    print_banner_start("profile/fetch");

    pids[0] = (int)getpid();
    pids[1] = (int)getppid();
    pmDelProfile(indom, 0, NULL);
    pmAddProfile(indom, 2, pids);

    printf("\n--- Check profile in context dump... ---\n");
    if ((sts = pmWhichContext()) < 0) {
	printf("%s: pmWhichContext: %s\n", pmGetProgname(), pmErrStr(sts));
	exit(1);
    }
    __pmDumpContext(stdout, sts, PM_INDOM_NULL);
    printf("--- End Check profile in context dump... ---\n");

    printf("\n--- Fetch Over Restricted Instance Domain ... ---\n");
    for (i=0; i < iterations; i++) {
	int j,k;

	sts = pmFetch(nmetrics, pmids, &result1);
	if (sts < 0) {
	    printf("%s: iteration %d : %s\n", pmGetProgname(), i, pmErrStr(sts));
	    exit(1);
	}
	__pmDumpResult(stdout, result1);


	for (j = 0; j < result1->numpmid; j++) {
	    pmValueSet *set = result1->vset[j];

	    if (set->numval != 2) {
		printf("%s: Error: num of inst == %d\n", pmGetProgname(), set->numval);
	    }        

	    for (k = 0; k < set->numval; k++) {
		pmValue *val = &set->vlist[k];
		if (val->inst != pids[0] && val->inst != pids[1]) {
		    printf("%s: Error: inst ids do not match pids\n", pmGetProgname());
		    exit(1);
		}        
	    } 
	}

	pmFreeResult(result1);
    }
    printf("--- End Fetch Over Restricted Instance Domain ... ---\n");



    printf("\n--- Fetch Over Entire Instance Domain ... ---\n");
    if (indom != PM_INDOM_NULL) {
	pmDelProfile(indom, 0, NULL);
	pmAddProfile(indom, all_n, all_inst);
    }
    sts = pmFetch(nmetrics, pmids, &result2);
    if (sts < 0) {
	printf("%s: fetch all %d instances : %s\n", pmGetProgname(), all_n, pmErrStr(sts));
	exit(1);
    }
    __pmDumpResult(stdout, result2);
    pmFreeResult(result2);
    printf("--- End Fetch Over Entire Instance Domain ... ---\n");

    print_banner_end("profile/fetch");

}

void
test_store(void)
{
    int sts;
    pmResult	*result;

    print_banner_start("store");

    sts = pmFetch(nmetrics, pmids, &result);
    if (sts < 0) {
	printf("%s: fetch failed : %s\n", pmGetProgname(), pmErrStr(sts));
	exit(1);
    }
    if ((sts = pmStore(result)) != -EACCES && sts != PM_ERR_PERMISSION) {
	printf("%s: Error: pmStore did not fail correctly\n", pmGetProgname());
	printf("Expected: %s\n", pmErrStr(-EACCES));
	printf("or: %s\n", pmErrStr(PM_ERR_PERMISSION));
	printf("Got:      %s\n", pmErrStr(sts));
	exit(1);
    }
    pmFreeResult(result);

    print_banner_end("store");
}

int
main(int argc, char **argv)
{
    int	sts;

    set_proc_fmt();
    printf("pid=%" FMT_PID " ppid=%" FMT_PID "\n", getpid(), getppid());
    getargs(argc, argv);

    if (pmnsfile != PM_NS_DEFAULT) {
	if ((sts = pmLoadASCIINameSpace(pmnsfile, 1)) < 0) {
	    printf("%s: Cannot load pmnsfile from \"%s\": %s\n", 
		    pmGetProgname(), pmnsfile, pmErrStr(sts));
	    exit(1);
	}
    }

    if ((sts = pmNewContext(PM_CONTEXT_HOST, host)) < 0) {
	printf("%s: Cannot connect to PMCD on host \"%s\": %s\n", 
		pmGetProgname(), host, pmErrStr(sts));
	exit(1);
    }

    test_PMNS();
    test_desc();
    test_instance();
    test_prof_fetch();
    if (!is_hotproc)
        test_store();

    if ((sts = pmWhichContext()) < 0) {
	printf("%s: pmWhichContext: %s\n", pmGetProgname(), pmErrStr(sts));
	exit(1);
    }
    pmDestroyContext(sts);

    exit(0);
}
