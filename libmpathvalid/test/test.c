#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <libudev.h>
#include "mpath_valid.h"

#define STRICT "strict"
#define SMART "smart"
#define GREEDY "greedy"
char *cmd;

void usage(void) {
	printf("Usage: %s <dev> ["STRICT"|"SMART"|"GREEDY"] [<wwid> ...]\n", cmd);
	exit(1);
}

char *print_claim(int val)
{
	switch(val) {
	case MPATH_IS_MAYBE_VALID:
		return "is a possible mpath path";
	case MPATH_IS_VALID_NO_CHECK:
		return "is a mpath path";
	case MPATH_IS_VALID:
		return "is a mpath path if new";
	case MPATH_IS_NOT_VALID:
		return "is not a mpath path";
	case MPATH_IS_ERROR:
		return "returned an error";
	default:
		return "returned an unexpected value";
	}
}

int get_mode(const char *uuid)
{
	if (strlen(uuid) == strlen(STRICT) &&
	    strncmp(uuid, STRICT, strlen(STRICT)) == 0)
		return MPATH_STRICT;
	else if (strlen(uuid) == strlen(SMART) &&
		 strncmp(uuid, SMART, strlen(SMART)) == 0)
		return MPATH_SMART;
	else if (strlen(uuid) == strlen(GREEDY) &&
		 strncmp(uuid, GREEDY, strlen(GREEDY)) == 0)
		return MPATH_GREEDY;
	else
		usage();
	/* never reached */
	return -1;
}

char *print_mode(unsigned int mode)
{
	switch (mode){
	case MPATH_STRICT:
		return STRICT;
	case MPATH_SMART:
		return SMART;
	case MPATH_GREEDY:
		return GREEDY;
	case MPATH_MODE_ERROR:
		return "error - not initialized";
	default:
		return "unknown";
	}
}

int main(int argc, char *argv[])
{
	char *wwid;
	const char **path_wwids = NULL;
	unsigned int mode = MPATH_DEFAULT;
	int i, ret;
	unsigned int nr_paths = 0;

	cmd = argv[0];

	if (argc < 2)
		usage();
	if (argc >= 3)
		mode = get_mode(argv[2]);

	if (argc > 3) {
		nr_paths = argc - 3;
		path_wwids = calloc(argc - 3, sizeof(char *));
		if (!path_wwids) {
			fprintf(stderr, "failed to allocte info list: %m\n");
			exit(1);
		}
	}
	if (mpathvalid_init(MPATH_LOG_PRIO_NOLOG, MPATH_LOG_STDERR) < 0) {
		fprintf(stderr, "failed to initialize mpathvalid\n");
		exit(1);
	}
	for (i = 3; i < argc; i++) {
		if (strlen(argv[i]) >= 128) {
			fprintf(stderr, "wwid \"%s\" longer than 128 bytes",
				argv[i]);
			exit(1);
		}
		path_wwids[i - 3] = argv[i];
		printf("path_wwids[%d] = %s\n", i - 3, path_wwids[i - 3]);
	}

	printf("config modes is %s\n", print_mode(mpathvalid_get_mode()));
	ret = mpathvalid_is_path(argv[1], mode, &wwid,
				 (const char **)path_wwids, nr_paths);
	printf("path \"%s\" %s\n", argv[1], print_claim(ret));
	printf("wwid is \"%s\"\n", wwid);
	mpathvalid_exit();
	return (ret <= 0);
}
