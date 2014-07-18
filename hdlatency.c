/*
 hdlatency.c - Measure HD latency (extended)
 Copyright (C) 2009-2011 by Arjen G. Lentz (arjen@openquery.com)
 Open Query (http://openquery.com), Brisbane QLD, Australia

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.

 ===
 Please read the following files
 - COPYING for GPL v3 licensing information
 - README for background/documentation
*/

#include <stdio.h>
#define _XOPEN_SOURCE 600
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#define __USE_GNU
#include <fcntl.h>
#include <sys/time.h>


#define VERSIONDATE         "2014-07-18"
#define AUTHOR              "arjen@openquery.com and O. Doucet (github: odoucet)"

#define MAX_BUFSIZE         (8*1024*1024)
#define LATENCYTEST_SECS    5
#define LATENCYTEST_MUL     100000

#define INNODB_PAGESIZE     16384

#define OPEN_FLAGS_DIRECT   (O_RDWR|O_CREAT|O_TRUNC|O_DIRECT|O_NOATIME)
#define OPEN_FLAGS_FSYNC    (O_RDWR|O_CREAT|O_TRUNC|O_NOATIME)



/*
    create specified filename for read/write of specified size
    if direct flag is set, open with O_DIRECT
    return fd
*/
int open_file (char *fname, long fsize ,int direct)
{
    int fd;

    if ((fd = open(fname,direct ? OPEN_FLAGS_DIRECT : OPEN_FLAGS_FSYNC,S_IRUSR|S_IWUSR)) < 0)
        fprintf(stderr,"cannot open %s for write (errno %d)\n",fname,errno);
    else
        ftruncate(fd,fsize);

    return (fd);
}/*open_file()*/



/*
    find out alignment for specified filename
    return alignment in bytes
*/
int get_alignment (char *fname)
{
    int alignment;

    // we do the whole alignment foo to be able to also do direct I/O tests
    if ((alignment = pathconf(fname,_PC_REC_XFER_ALIGN)) < 1)
        fprintf(stderr,"pathconf(%s) alignment error (errno %d)\n",fname,errno);

    return (alignment);
}/*get_alignment()*/



/*
    allocate buffer of specified size, aligned (in case we want to to direct I/O)
    fill buffer with /dev/urandom (and not a fairly arbitrary, 0s or 1s)
    because compressed FS will drastically change results.
    return ptr to newly allocated buffer or NULL
*/
char *alloc_buf (long msize, int alignment)
{
    int res;
    char *buf;
    FILE *fp;

    if ((res = posix_memalign((void *) &buf,alignment,msize)) != 0) {
        fprintf(stderr,"cannot allocate aligned buffer of %ld bytes (err %d)\n",msize,res);
        return (NULL);
    }

    fp = fopen("/dev/urandom", "r");
    fread(buf, MAX_BUFSIZE, 1, fp);
    fclose(fp);

    return (buf);
}/*alloc_buf()*/



/*
    call timeofday()
    fill tv
    return 1 for success, 0 for failure
*/
int get_timer (struct timeval *tv)
{
    if (gettimeofday(tv,NULL) < 0) {
        fprintf(stderr,"gettimeofday() failed (errno %d)\n",errno);
        return (0);
    }

    return (1);
}/*get_timer()*/



/*
    run sequential read (writeops=0) or write (writeops=1) tests for specified block size
    run test for at most max_seconds, or end of file (whichever comes first)
*/
int run_sequential_one (char *label, int writeops, int fd, long fsize, char *buf,
                        long msize, int alignment, int direct, int max_seconds, int timeofday_latency,
                        long iosize)
{
    long bytes_left;
    int bytes_io;
    struct timeval tvstart, tvend, tvdiff;
    long long num_iterations;
    long long usecs;

    // start of file and fsync before starting timer
    lseek(fd,0,SEEK_SET);
    fsync(fd);

    fprintf(stderr,"SEQ %s iosize=%ld\r", writeops ? "WRONLY" : "RDONLY", iosize);

    if (!get_timer(&tvstart))
        return (0);

    bytes_left = fsize;
    num_iterations = 0;
    do {
        if ((bytes_io = (writeops ? write(fd,buf,iosize) : read(fd,buf,iosize))) < 0) {
            fprintf(stderr,"cannot %s to file (errno %d)\n",writeops ? "write" : "read",errno);
            return (0);
        }

        // fsync on write if needed
        if (writeops && !direct)
            fsync(fd);

        if (!get_timer(&tvend))
            return (0);

        bytes_left -= bytes_io;
        num_iterations++;
        timersub(&tvend,&tvstart,&tvdiff);
    } while (tvdiff.tv_sec < max_seconds && bytes_left >= iosize);

    usecs = ((tvdiff.tv_sec * 1000000LL) + tvdiff.tv_usec) - (((long long) num_iterations * timeofday_latency) / LATENCYTEST_MUL);
    if (usecs < 1)
        usecs = 1;

    printf("%s,SEQ,%s,%ld,%ld,%d,%d,%d,%ld,%lld,%lld,%ld,%ld\n",
        label, writeops ? "WRONLY" : "RDONLY",
        fsize, msize, alignment, direct, max_seconds,
        iosize, num_iterations, usecs,
        (long) (1000000LL / (usecs / num_iterations)),
        (long) (usecs / num_iterations));

    return (1);
}/*run_sequential_one()*/



/*
    run sequential read (writeops=0) or write (writeops=1) tests for different block sizes
    run each test for at most max_seconds, or end of file (whichever comes first)
    for direct I/O, start block size at alignment, otherwise start at 1
*/
int run_sequential_all (char *label, int writeops, int fd, long fsize, char *buf,
                        long msize, int alignment, int direct, int max_seconds, int timeofday_latency)
{
    long iosize;

    fprintf(stderr,"= %s SEQ %-6s (fsize=%ld,msize=%ld,alignment=%d,direct=%d,max_seconds=%d)\n",
        label, writeops ? "WRONLY" : "RDONLY",fsize,msize,alignment,direct,max_seconds);

    for (iosize = direct ? alignment : 1; iosize <= msize; iosize <<= 1) {
        if (!run_sequential_one(label,writeops,fd,fsize,buf,
                                msize,alignment,direct,max_seconds,timeofday_latency,
                                iosize))
        return (0);
    }

    return (1);
}/*run_sequential_all()*/



/*
    run random read (writeops=0), write (writeops=1), read/write alternately (writeops=2)
    tests for specified block size, for max_seconds
*/
int run_random_one (char *label, int writeops, int fd, long fsize, char *buf,
                    long msize, int alignment, int direct, int max_seconds, int timeofday_latency,
                    long iosize)
{
    off_t pos;
    struct timeval tvstart, tvend, tvdiff;
    long long num_iterations;
    long long usecs;
    int rw;

    // seed random for consistency between our testruns
    srand(1);

    // fsync before starting timer
    lseek(fd,0,SEEK_SET);
    fsync(fd);

    fprintf(stderr,"RND %s iosize=%ld\r", writeops ? (writeops==2 ? "RDWR" : "WRONLY") : "RDONLY", iosize);

    if (!get_timer(&tvstart))
       return (0);

    num_iterations = 0;
    rw = (writeops == 2) ? 1 : writeops;
    do {
        // Yes, rand() % N is dodgy, but it'll do for this purpose
        pos = (off_t) ((rand() % ((fsize - iosize) / iosize)) * iosize);
        if (lseek(fd,pos,SEEK_SET) < 0) {
            fprintf(stderr,"seek error for ofs=%ld (errno %d)\n",(long) pos,errno);
            return (0);
        }

        if (writeops == 2)
            rw = rw ? 0 : 1;    // alternate r/w

        if ((rw ? write(fd,buf,iosize) : read(fd,buf,iosize)) < 0) {
            fprintf(stderr,"cannot %s file (errno %d)\n",writeops ? "write" : "read",errno);
            return (0);
        }

        if (rw && !direct)
            fsync(fd);

        if (!get_timer(&tvend))
            return (0);

        num_iterations++;
        timersub(&tvend,&tvstart,&tvdiff);
    } while (tvdiff.tv_sec < max_seconds);

    usecs = ((tvdiff.tv_sec * 1000000LL) + tvdiff.tv_usec) - (((long long) num_iterations * timeofday_latency) / LATENCYTEST_MUL);
    if (usecs < 1)
        usecs = 1;

    printf("%s,RND,%s,%ld,%ld,%d,%d,%d,%ld,%lld,%lld,%ld,%ld\n",
        label, writeops ? (writeops==2 ? "RDWR" : "WRONLY") : "RDONLY",
        fsize, msize, alignment, direct, max_seconds,
        iosize, num_iterations, usecs,
        (long) (1000000LL / (usecs / num_iterations)),
        (long) (usecs / num_iterations));

    return (1);
}/*run_random_one()*/



/*
    run random read (writeops=0), write (writeops=1), read/write alternately (writeops=2) tests for different block sizes
    run each test for max_seconds
    for direct I/O, start block size at alignment, otherwise start at 1
*/
int run_random_all (char *label, int writeops, int fd, long fsize, char *buf,
                    long msize, int alignment, int direct, int max_seconds, int timeofday_latency)
{
    long iosize;

    fprintf(stderr,"= %s RND %-6s (fsize=%ld,msize=%ld,alignment=%d,direct=%d,max_seconds=%d)\n",
        label, writeops ? (writeops==2 ? "RDWR" : "WRONLY") : "RDONLY",fsize,msize,alignment,direct,max_seconds);

    for (iosize = direct ? alignment : 1; iosize <= msize; iosize <<= 1) {
        if (!run_random_one(label,writeops,fd,fsize,buf,
                            msize,alignment,direct,max_seconds,timeofday_latency,
                            iosize))
            return (0);
    }

    return (1);
}/*run_random_all()*/



int main (int argc, char *argv[])
{
    char *label, *fname;
    long max_filesize;
    int max_seconds;
    struct timeval tvstart, tvend, tvdiff;
    long i;
    int timeofday_latency;
    int fd;
    int alignment;
    int opt_quick = 0;
    char *buf = NULL;

    // turn off output buffering so we can see what's going on
    // also keeps output in sync in case we write stdio/stderr to same destination
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    fprintf(stderr,"%s %s by arjen@openquery.com, modified by O. Doucet (github: odoucet)\n\n",argv[0],VERSIONDATE);

    if (argc < 5 || argc > 6) {
        fprintf(stderr,"Usage: %s [--quick] <testlabel> <filename> <MBfilesize> <seconds>\n",argv[0]);
        fprintf(stderr,"  --quick only tests with 16K blocks (InnoDB page size)\n");
        fprintf(stderr,"  Sample command line using a 1GB file with 60 secs/test:\n");
        fprintf(stderr,"  %s MyTestHD /mntpoint/testfile 1024 60 >mytesthd.csv\n",argv[0]);
        exit (0);
    }

    i = 1;
    if (!strcmp(argv[i],"--quick")) {
        opt_quick = 1;
        i++;
    }

    label           = strdup(argv[i++]);
    fname           = strdup(argv[i++]);
    max_filesize    = atol(argv[i++]) * (1024*1024);  // convert to MBs
    max_seconds     = atoi(argv[i++]);

    if (max_filesize < MAX_BUFSIZE*4) {
        fprintf(stderr, "Filesize should be at least %.0f MB (or patch hdlatency.c and decrease MAX_BUFSIZE)\n", (float) MAX_BUFSIZE*4/1024.0/1024.0);
        exit(1);
    }


    // File header
    printf("; %s %s by %s, operating on file '%s'\n",argv[0],VERSIONDATE,AUTHOR,fname);


    // timeofday() latency test, for compensation of the testruns
    fprintf(stderr,"* Calculating timekeeping latency over %d seconds...",LATENCYTEST_SECS);
    i = 0;
    get_timer(&tvstart);
    do {
        get_timer(&tvend);
        timersub(&tvend,&tvstart,&tvdiff);
        i++;
    } while (tvdiff.tv_sec < LATENCYTEST_SECS);
    timeofday_latency = ((tvdiff.tv_sec * 1000000) + tvdiff.tv_usec) / (i / LATENCYTEST_MUL);
    fprintf(stderr," %d usecs per %d calls\n", timeofday_latency, LATENCYTEST_MUL);


    // CSV header
    printf("label,iotype,rw,fsize,msize,alignment,direct,max_seconds,iosize,num_iterations,usecs,iterations_per_second,avg_usecs_per_iteration\n");


    // ==========
    // Direct I/O
    if (
        (fd = open_file(fname,max_filesize,1 /*direct*/)) < 0 ||
        (alignment = get_alignment(fname)) < 1 ||
        (opt_quick && alignment > INNODB_PAGESIZE) ||
        (buf = alloc_buf(MAX_BUFSIZE,alignment)) == NULL
        )
        fprintf(
            stderr,
            "DIRECT I/O init fail (fd=%d alignment=%d opt_quick=%d alignment>INNODB_PAGE_SIZE=%d buf_is_null=%d)\n", 
            fd, alignment, opt_quick, (alignment > INNODB_PAGESIZE), (buf == NULL)
        );
    else
        if (
            (opt_quick &&
             (
                !run_sequential_one(label,    1 /*wronly*/  ,fd,max_filesize,buf,MAX_BUFSIZE,alignment,1 /*direct*/,max_seconds,timeofday_latency,INNODB_PAGESIZE) ||
                !run_sequential_one(label,    0 /*rdonly*/  ,fd,max_filesize,buf,MAX_BUFSIZE,alignment,1 /*direct*/,max_seconds,timeofday_latency,INNODB_PAGESIZE) ||
                !run_random_one(    label,    1 /*wronly*/  ,fd,max_filesize,buf,MAX_BUFSIZE,alignment,1 /*direct*/,max_seconds,timeofday_latency,INNODB_PAGESIZE) ||
                !run_random_one(    label,    0 /*rdonly*/  ,fd,max_filesize,buf,MAX_BUFSIZE,alignment,1 /*direct*/,max_seconds,timeofday_latency,INNODB_PAGESIZE) ||
                !run_random_one(    label,    2 /*rdwr*/    ,fd,max_filesize,buf,MAX_BUFSIZE,alignment,1 /*direct*/,max_seconds,timeofday_latency,INNODB_PAGESIZE)
             )
            ) ||

            (!opt_quick &&
             (
                !run_sequential_all(label,    1 /*wronly*/  ,fd,max_filesize,buf,MAX_BUFSIZE,alignment,1 /*direct*/,max_seconds,timeofday_latency) ||
                !run_sequential_all(label,    0 /*rdonly*/  ,fd,max_filesize,buf,MAX_BUFSIZE,alignment,1 /*direct*/,max_seconds,timeofday_latency) ||
                !run_random_all(    label,    1 /*wronly*/  ,fd,max_filesize,buf,MAX_BUFSIZE,alignment,1 /*direct*/,max_seconds,timeofday_latency) ||
                !run_random_all(    label,    0 /*rdonly*/  ,fd,max_filesize,buf,MAX_BUFSIZE,alignment,1 /*direct*/,max_seconds,timeofday_latency) ||
                !run_random_all(    label,    2 /*rdwr*/    ,fd,max_filesize,buf,MAX_BUFSIZE,alignment,1 /*direct*/,max_seconds,timeofday_latency)
             )
            )
           )
        fprintf(stderr,"run fail\n");

    if (fd >= 0)
        close(fd);
    unlink(fname);
    if (buf != NULL)
        free(buf);


    // ==========
    // fsync()
    if (
        (fd = open_file(fname,max_filesize,0 /*not direct*/)) < 0 ||
        (alignment = get_alignment(fname)) < 1 ||
        (opt_quick && alignment > INNODB_PAGESIZE) ||
        (buf = alloc_buf(MAX_BUFSIZE,alignment)) == NULL
        )
        fprintf(
            stderr,
            "NON DIRECT I/O init fail (fd=%d alignment=%d opt_quick=%d alignment>INNODB_PAGE_SIZE=%d buf_is_null=%d)\n", 
            fd, alignment, opt_quick, (alignment > INNODB_PAGESIZE), (buf == NULL)
        );
    else   
        if (
            // for non-direct I/O...
            // - sequential read silly as it runs on fs cache
            // - random read silly as it mostly runs on fs cache (perhaps we can rectify this to measure reads)
            // - random r/w ditto
            (opt_quick &&
             (
                !run_sequential_one(label,  1 /*wronly*/  ,fd,max_filesize,buf,MAX_BUFSIZE,alignment,0 /*not direct*/,max_seconds,timeofday_latency,INNODB_PAGESIZE) ||
                !run_random_one(    label,  1 /*wronly*/  ,fd,max_filesize,buf,MAX_BUFSIZE,alignment,0 /*not direct*/,max_seconds,timeofday_latency,INNODB_PAGESIZE)
             )
            ) ||

            (!opt_quick &&
             (
                !run_sequential_all(label,  1 /*wronly*/  ,fd,max_filesize,buf,MAX_BUFSIZE,alignment,0 /*not direct*/,max_seconds,timeofday_latency) ||
                !run_random_all(    label,  1 /*wronly*/  ,fd,max_filesize,buf,MAX_BUFSIZE,alignment,0 /*not direct*/,max_seconds,timeofday_latency)
             )
            )
           )
        fprintf(stderr,"run fail\n");

    close(fd);   
    unlink(fname);
    if (buf != NULL)
        free(buf);

    fprintf(stderr, "Benchmark finished.\n");

    exit (0);
}/*main()*/



/* end of hdlatency.c */
