
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "pwq_lib.h"

pw_pool_t pwp;

int sleeper_cell_work (uint64_t client_info, uint64_t arg)
{
	int ret = 0;
	unsigned int nsec = (unsigned int)arg;

	pw_assert_err (client_info <= 10, ret, return -2, \
			"sc found client_info: %lu\n", client_info);

	printf ("sleeping for %u seconds as client %lu requested\n", nsec, client_info);
	sleep (nsec);
	printf ("Done the work for client %lu!\n", client_info);

	return ret;
}

int main (int argc, char **argv)
{
	int ret = 0;
	char choice;
	int alive = 1;

	ret = pw_setup_pool ("test_pwq", 2, 1, 2, &pwp);

	pw_assert_err (ret == 0, ret, return ret, \
			"Unable to setup %s\n", "test_pwq");

	printf ("enter value in range 1-10 to start \
			sleeper-work for that many seconds; 0 to exit");
	while (alive) {
		printf ("\npwq > ");
		while (scanf (" %c", &choice) != 1) {
			printf ("need valid int\n");
			fflush(stdin);
		}
		switch (choice) {
			case '0':
				pw_destroy_pool (&pwp);
				alive = 0;
				break;
			default:
				if (choice < '0' || choice > '9') {
					printf ("allowed range: 0 to 9\n");
					break;
				}
				ret = pw_queue_work (&pwp, sleeper_cell_work, \
						/*client_info*/choice - '0', /*data to process*/choice - '0');
				pw_assert_warn (ret == 0, ret, ret = 0, \
						"Couldn't activate sleeper_cell for operation: %d", choice);
				break;
		}
	}
	return ret;
}

