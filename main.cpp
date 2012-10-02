#include <stdio.h>
#include <string.h>

#include "repack.h"

int main(int argc, char *argv[])
{
	if (argc != 4) {
		printf("Usage: %s (inflate|deflate) <zipfile> <outfile>\n", argv[0]);
		return -1;
	}

	if (!strcmp(argv[1], "inflate")) {
		printf("inflate mode\n");
		return flatten(argv[3], argv[2]);
	} else if (!strcmp(argv[1], "deflate")) {
		printf("deflate mode\n");
		return squeeze(argv[3], argv[2]);
	}
	printf("Must specify inflate or deflate\n");
	return -1;

}
