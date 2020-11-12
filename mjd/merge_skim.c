#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "MJDSort.h"

#define VERBOSE 1

int main(int argc, char **argv) {

  MJDetInfo  Dets[NMJDETS];
  MJRunInfo  runInfo;
  int        argn=1;
  FILE       *f_in, *f_out;

  SavedData2 **sd;
  int     nsd = 0;  // number of data to be read from one file
  int     sdchunk = 1000000; // number of SavedData events to malloc at one time
  int     numsd = 0, maxsd = sdchunk;  // number of saved data, and pointer to saved data id
  int     i, sd_version = 1;



  if (argc < 2) {
    fprintf(stderr, "\nusage: %s fname1_in fname2_in [fname3_in ...] \n\n", argv[0]);
    return -1;
  }
  /* malloc initial space for SavedData */
  if ((sd = malloc(sdchunk*sizeof(*sd))) == NULL ||
      (sd[0] = malloc(sdchunk*sizeof(**sd))) == NULL) {
    printf("ERROR in merge_skim.c; cannot malloc SavedData!\n");
    exit(-1);
  }
  for (i=1; i<sdchunk; i++) sd[i] = sd[i-1] + 1;
  nsd = sdchunk;

  /* open skim data file as input */
  while (argn < argc && (f_in = fopen(argv[argn], "r"))) {
    printf("Reading skim file %s\n", argv[argn]);

    // read saved skim data from f_in
    fread(&nsd, sizeof(int), 1, f_in);
    if (nsd == -2) {
      sd_version = 2;
      fread(&nsd, sizeof(int), 1, f_in);
    }
    fread(&Dets[0], sizeof(Dets[0]), NMJDETS, f_in);
    fread(&runInfo, sizeof(runInfo) - 8*sizeof(int), 1, f_in);
    if (runInfo.idNum == 0) {
      runInfo.flashcam = 1;
      fread(&(runInfo.flashcam), 8*sizeof(int), 1, f_in);
    }

    printf("Skim data mode = %d;  %d detectors, %d skimmed events\n",
           sd_version, runInfo.nGe, nsd);
    if (sd_version == 1) {
      printf("Error: This version of merge_skim works only for newer type skim files.\n");
      exit(-1);
    } else {   // sd_version == 2
      while (numsd + nsd >= maxsd) {
        //printf("1  nsd: %d  numsd: %d    maxsd: %d\n", nsd, numsd, maxsd); fflush(stdout);
        fread(sd[numsd], sizeof(**sd), maxsd - numsd, f_in);
        nsd -= maxsd - numsd;
        numsd = maxsd;
        if ((sd = realloc(sd, (maxsd + sdchunk)*sizeof(*sd))) == NULL ||
            (sd[maxsd] = malloc(sdchunk*sizeof(**sd))) == NULL) {
          printf("ERROR in skim.c; cannot realloc SavedData! nsd = %d\n", nsd);
          exit(-1);
        }
        maxsd += sdchunk;
        //printf("1c  nsd: %d  numsd: %d    maxsd: %d\n", nsd, numsd, maxsd); fflush(stdout);
        for (i=numsd+1; i<maxsd; i++) sd[i] = sd[i-1]+1;
      }
      //printf("2  nsd: %d  numsd: %d    maxsd: %d\n", nsd, numsd, maxsd); fflush(stdout);
      fread(sd[numsd], sizeof(**sd), nsd, f_in);
      numsd += nsd;
      //printf("3  nsd: %d  numsd: %d    maxsd: %d\n", nsd, numsd, maxsd); fflush(stdout);
    }
    printf(" Skim is data from runs starting at number %d from file %s\n"
           "  Total events now %d\n",
           runInfo.runNumber, runInfo.filename, numsd);

    argn++;
    fclose(f_in);
  }
  // save skim data (SavedData) to disk
  printf("\n >>> Writing skim file merged_skim.dat\n"); fflush(stdout);
  f_out = fopen("merged_skim.dat", "w");
  i = -2;
  fwrite(&i, sizeof(int), 1, f_out);    // flag to tell reading programs to use SavedData2 instead of SavedData
  fwrite(&numsd, sizeof(int), 1, f_out);
  fwrite(&Dets[0], sizeof(Dets[0]), NMJDETS, f_out);
  if (runInfo.flashcam) {
    runInfo.idNum = 0;
    fwrite(&runInfo, sizeof(runInfo), 1, f_out);  // for backwards compatibility
  } else {
    fwrite(&runInfo, sizeof(runInfo) - 8*sizeof(int), 1, f_out);
  }

  for (i = 0; i < numsd; i += sdchunk) {
    if (numsd - i < sdchunk) {
      fwrite(sd[i], sizeof(**sd), numsd - i, f_out);
    } else {
      fwrite(sd[i], sizeof(**sd), sdchunk, f_out);
    }
  }
  fclose(f_out);
  printf(" Wrote %d skimmed events of size %lu to skim.dat\n\n", numsd, sizeof(**sd));

  return 0;
}