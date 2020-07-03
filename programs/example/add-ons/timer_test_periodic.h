#include <getopt.h>

#define APP_EO_NAME	"testEO"
#define DEF_TMO_DATA	100 /* per core, MAX_TMO_DATA * sizeof(tmo_data) */
#define MAX_TMO_BYTES	1000000000ULL /* sanity limit 1GB per core */
#define STOP_THRESHOLD	90 /* % of full buffer */
#define MAX_CORES	64
#define INIT_WAIT	5 /* startup wait, HBs */
#define MEAS_PERIOD	5 /* freq meas HBs */
#define DEF_MIN_PERIOD	10 /* min period default, N * res */
#define USE_TIMER_STAT  0 /* 0 if EM does not support timer ack statistics */
#define EXTRA_PRINTS	0 /* dev option, normally 0 */

const struct option longopts[] = {
	{"num-tmo",		required_argument, NULL, 'n'},
	{"resolution",		required_argument, NULL, 'r'},
	{"period",		required_argument, NULL, 'p'},
	{"max-period",		required_argument, NULL, 'm'},
	{"min-period",		required_argument, NULL, 'l'},
	{"clk",			required_argument, NULL, 'c'},
	{"write",		optional_argument, NULL, 'w'},
	{"num-runs",		required_argument, NULL, 'x'},
	{"tracebuf",		required_argument, NULL, 't'},
	{"extra-work",		required_argument, NULL, 'e'},
	{"background-job",	required_argument, NULL, 'j'},
	{"noskip",		no_argument, NULL, 's'},
	{"api-prof",		no_argument, NULL, 'a'},
	{"dispatch-prof",	no_argument, NULL, 'd'},
	{"job-prof",		no_argument, NULL, 'b'},
	{"use-cpu-cycle",	optional_argument, NULL, 'g'},
	{"help",		no_argument, NULL, 'h'},
	{NULL, 0, NULL, 0}
};

const char *shortopts = "n:r:p:m:l:c:w::x:t:e:j:sadbg::h";
/* descriptions for above options, keep in sync! */
const char *descopts[] = {
	"Number of concurrent timers to create",
	"Resolution of test timer (ns)",
	"Period of periodic test timer (ns). 0 for random",
	"Maximum period (ns)",
	"Minimum period (ns, only used for random tmo)",
	"Clock source (integer. See event_machine_timer_hw_specific.h)",
	"Write raw trace data in csv format to given file e.g.-wtest.csv (default stdout)",
	"Number of test runs, 0 to run forever",
	"Trace buffer size (events per core). Optional stop threshold % e.g. -t100,80 to stop 80% before full",
	"Extra work per tmo: -e1,20,50 e.g. min_us,max_us,propability % of work",
	"Extra background job: -j2,20,500,10 e.g. num,time_us,total_kB,chunk_kB",
	"Create timer with NOSKIP option",
	"Measure API calls",
	"Include dispatcher trace (EO enter-exit)",
	"Include bg job profile (note - can fill buffer quickly)",
	"Use CPU cycles instead of ODP time. Optionally give frequency (hz)",
	"Print usage and exit",
	NULL
};

const char *instructions =
"Controlled by command line arguments. Main purpose is to manually test\n"
"periodic timer accuracy and behaviour optionally under (over)load.\n"
"API overheads can also be measured\n"
"\nTwo EM timers are created. One for a heartbeat driving test states. Second\n"
"timer is used for testing the periodic timeouts. It can be created with\n"
"given attributes to also test limits.\n\n"
"Test runs in states:\n"
"	STATE_INIT	let some time pass before starting to settle down\n"
"	STATE_MEASURE	measure timer tick frequency against linux clock\n"
"	STATE_STABILIZE finish all prints before starting run\n"
"	STATE_RUN	timeouts creared and measured\n"
"	STATE_COOLOFF	first core hitting trace buffer limit sets cooling,\n"
"			i.e. coming timeout(s) are cancelled but not analyzed\n"
"	STATE_ANALYZE	Statistics and trace file generation, restart\n"
"\nBy default there is no background load and the handling of incoming\n"
"timeouts (to high priority parallel queue) is minimized. Extra work can be\n"
"added in two ways:\n\n"
"1) --extra-work min us, max us, propability %\n"
"	this will add random delay between min-max before calling ack().\n"
"	Delay is added with the given propability (100 for all tmos)\n"
"	e.g. -e10,100,50 to add random delay between 10 and 100us with 50%\n"
"	propability\n"
"2) --background-job  num,length us,total_kB,chunk_kB\n"
"	this adds background work handled via separate low priority parallel\n"
"	queue. num events are sent at start. Receiving touches given amount\n"
"	of data (chunk at a time) for given amount of time and\n"
"	then sends it again\n"
"	e.g. -j1,10,500,20 adds one event of 10us processing over 20kB data\n"
"	randomly picked from 500kB\n\n"
"Test can write a file of measured timings (-w). It is in CSV format and can\n"
"be imported e.g. to excel for plotting. -w without name prints to stdout\n";

typedef enum e_op {
	OP_TMO,
	OP_HB,
	OP_STATE,
	OP_CANCEL,
	OP_WORK,
	OP_ACK,
	OP_BGWORK,
	OP_PROF_ACK,	/* linux time used as tick diff for each PROF */
	OP_PROF_DELETE,
	OP_PROF_CREATE,
	OP_PROF_SET,
	OP_PROF_ENTER_CB,
	OP_PROF_EXIT_CB
} e_op;
const char *op_labels[] = {
	"TMO",
	"HB",
	"STATE",
	"CANCEL",
	"WORK",
	"ACK",
	"BG-WORK",
	"PROF-ACK",
	"PROF-DEL",
	"PROF-CREATE",
	"PROF-SET",
	"PROF-ENTER_CB",
	"PROF-EXIT_CB"
};

typedef union time_stamp { /* to work around ODP time type vs CPU cycles */
	uint64_t u64;
	odp_time_t odp;
} time_stamp;
typedef struct tmo_trace {
		int id;
		e_op op;
		uint64_t tick;
		time_stamp ts;
		time_stamp linuxt;
		int count;
} tmo_trace;

#define RND_STATE_BUF   32
typedef struct rnd_state_t {
	struct random_data rdata;
	char rndstate[RND_STATE_BUF];
} rnd_state_t;

typedef struct core_data {
	int count ODP_ALIGNED_CACHE;
	tmo_trace *trc;
	int cancelled;
	int jobs;
	int jobs_deleted;
	rnd_state_t rng;
	time_stamp enter;
	time_stamp acc_time;
} core_data;

typedef enum e_cmd {
	CMD_HEARTBEAT,
	CMD_TMO,
	CMD_DONE,
	CMD_BGWORK
} e_cmd;

typedef struct app_msg_t {
	e_cmd command;
	int count;
	em_tmo_t tmo;
	int id;
	uint64_t arg;

} app_msg_t;

typedef enum e_state {
	STATE_INIT,	/* before start */
	STATE_MEASURE,	/* measure timer freq */
	STATE_STABILIZE,/* finish all printing before tmo setup */
	STATE_RUN,	/* timers running */
	STATE_COOLOFF,	/* cores cancel timers */
	STATE_ANALYZE	/* timestamps analyzed */
} e_state;

typedef struct tmo_setup {
	time_stamp start_ts;
	uint64_t start;
	uint64_t period_ns;
	uint64_t ticks;
	uint64_t ack_late;
} tmo_setup;