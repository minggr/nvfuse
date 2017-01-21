/*
*	NVFUSE (NVMe based File System in Userspace)
*	Copyright (C) 2016 Yongseok Oh <yongseok.oh@sk.com>
*	First Writing: 30/10/2016
*
* This program is free software; you can redistribute it and/or modify it
* under the terms and conditions of the GNU General Public License,
* version 2, as published by the Free Software Foundation.
*
* This program is distributed in the hope it will be useful, but WITHOUT
* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
* FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
* more details.
*/

#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>

#include "nvfuse_core.h"
#include "nvfuse_api.h"
#include "nvfuse_io_manager.h"
#include "nvfuse_malloc.h"
#include "nvfuse_gettimeofday.h"

#define DEINIT_IOM	1
#define UMOUNT		1

#define NUM_ELEMENTS(x) (sizeof(x)/sizeof(x[0]))

#define MB (1024*1024)
#define GB (1024*1024*1024)
#define TB ((s64)1024*1024*1024*1024)

#define RANDOM		1
#define SEQUENTIAL	0

#define IS_NOTHING 0
#define IS_OPEN_CLOSE 1
#define IS_READDIR 2
#define IS_UNLINK 3
#define IS_CREATE 4
#define IS_RENAME 5
#define IS_MKDIR 6
#define IS_RMDIR 7

int perf_aio(struct nvfuse_handle *nvh, s64 file_size, s32 block_size, s32 is_rand, s32 is_read, s32 direct, s32 qdepth)
{	
	struct timeval tv;
	char str[FNAME_SIZE];
	s32 res;

	sprintf(str, "file_allocate_test");

	gettimeofday(&tv, NULL);
	
	res = nvfuse_aio_test_rw(nvh, str, file_size, block_size, qdepth, is_read ? READ : WRITE, direct, is_rand);
	if (res < 0)
	{
		printf(" Error: aio write test \n");
		goto AIO_ERROR;
	}
	printf(" nvfuse aio write through %.3f MB/s\n", (double)file_size / MB / time_since_now(&tv));

AIO_ERROR:
	res = nvfuse_rmfile_path(nvh, str);
	if (res < 0)
	{
		printf(" Error: rmfile = %s\n", str);
		return -1;
	}	

	return 0;
}

static int perf_metadata(struct nvfuse_handle *nvh, s32 meta_check, s32 count)
{	
	char str[FNAME_SIZE];
	s32 res;

	sprintf(str, "file_allocate_test");

	res = nvfuse_metadata_test(nvh, str, meta_check,count);
	if (res < 0)
	{
		printf(" Error: aio write test \n");
		goto META_ERROR;	
    }
    return NVFUSE_SUCCESS;

META_ERROR:

	return NVFUSE_ERROR;
}


void perf_usage(char *cmd)
{
	printf("\nOptions for NVFUSE application: \n\n");
    printf("   for metadata performance checking ... \n");
	printf("\t\t-M: metadata type to measure (e.g., open_close , readdir , unlink , create , rename , mkdir , rmdir)\n");
    printf("\t\t-C: repetition to measure metadata intensice operation\n");
    printf("   for metadata performance checking ... \n");
	printf("\t\t-S: file size (in MB)\n");
	printf("\t\t-B: block size (in B)\n");
	printf("\t\t-E: ioengine (e.g., libaio, sync)\n");
	printf("\t\t-Q: qdepth \n");
	printf("\t\t-R: random (e.g., rand or sequential)\n");
	printf("\t\t-D: direct I/O \n");
	printf("\t\t-W: write workload (e.g., write or read)\n");
}

#define AIO 1
#define SYNC 2

int main(int argc, char *argv[])
{
	struct nvfuse_handle *nvh;	
	int ret = 0;

	int file_size;
	int block_size;
	int ioengine;
	int qdepth;
	int is_rand = 0; /* sequential set to as default */
	int direct_io = 0; /* buffered I/O set to as default */
	int is_write = 0; /* write workload set to as default */
    int count = 0;
   
    int meta_check = IS_NOTHING; 
	char op;
		
	int core_argc = 0;
	char *core_argv[128];		
	int app_argc = 0;
	char *app_argv[128];

	if (argc == 1)
	{
		goto INVALID_ARGS;
	}

	/* distinguish cmd line into core args and app args */
	nvfuse_distinguish_core_and_app_options(argc, argv, 
											&core_argc, core_argv, 
											&app_argc, app_argv);
	
	/* optind must be reset before using getopt() */
	optind = 0;
	while ((op = getopt(app_argc, app_argv, "M:C:S:B:E:Q:RDWOAUCNI")) != -1) {
		switch (op) {
        case 'M':
            if (!strcmp(optarg, "open_close")){
                meta_check = IS_OPEN_CLOSE;	
			}else if (!strcmp(optarg, "readdir")){
                meta_check = IS_READDIR;
			}else if (!strcmp(optarg, "unlink")){
                meta_check = IS_UNLINK;
            }else if (!strcmp(optarg, "create")){
                meta_check = IS_CREATE;   
            }else if (!strcmp(optarg, "rename")){
                meta_check = IS_RENAME;
            }else if (!strcmp(optarg, "mkdir")){
                meta_check = IS_MKDIR;
            }else if (!strcmp(optarg, "rmdir")){
                meta_check = IS_RMDIR;
            }else{
				fprintf( stderr, " Invalid ioengine type = %s", optarg);
				goto INVALID_ARGS;
            }
			break;
        case 'C':
            count = atoi(optarg);
		case 'S':		
			file_size = atoi(optarg);
			break;
		case 'B':
			block_size = atoi(optarg);
			if (block_size % CLUSTER_SIZE)
			{
				printf(" Error: block size (%d) is not alinged with 4KB\n", block_size);
				goto INVALID_ARGS;
			}
			break;
		case 'E':
			if (!strcmp(optarg, "libaio"))
			{
				ioengine = AIO;
			}
			else if (!strcmp(optarg, "sync"))
			{
				ioengine = SYNC;
			}
			else
			{
				fprintf( stderr, " Invalid ioengine type = %s", optarg);
				goto INVALID_ARGS;
			}
			break;
		case 'Q':			
			qdepth = atoi(optarg);
			if (qdepth == 0)
			{
				fprintf(stderr, " Invalid qdepth = %d\n", qdepth);
				goto INVALID_ARGS;
			}
			break;
		case 'R':
			is_rand = 1;
			break;
		case 'D':
			direct_io = 1;
			break;
		case 'W':
			is_write = 1;
			break;
		default:
			goto INVALID_ARGS;
		}
	}
	
	/* create nvfuse_handle with user spcified parameters */
	nvh = nvfuse_create_handle(NULL, core_argc, core_argv);
	if (nvh == NULL)
	{
		fprintf(stderr, "Error: nvfuse_create_handle()\n");
		return -1;
	}

	printf("\n");

    if(meta_check != IS_NOTHING){
        ret = perf_metadata(nvh,meta_check,count); 
        if(ret == NVFUSE_ERROR)
            goto RET;
    }else{
        if (ioengine == AIO)
            ret = perf_aio(nvh, ((s64)file_size * MB), block_size, is_rand, is_write ? WRITE : READ, direct_io, qdepth);
        else
            printf(" sync io is not supported \n");;
    }

RET:;
	nvfuse_destroy_handle(nvh, DEINIT_IOM, UMOUNT);

	return ret;

INVALID_ARGS:;
	nvfuse_core_usage(argv[0]);
	perf_usage(argv[0]);
	nvfuse_core_usage_example(argv[0]);
	return -1;
}
