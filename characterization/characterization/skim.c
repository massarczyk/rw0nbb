#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "MJDSort.h"

#define VERBOSE 1
#define E_THRESH  1500  // threshold in keV, for speed of processing;
                        // should be less than 1550 to get DE peak for A/E

/*  ------------------------------------------------------------ */

int fitter(float *pars, float *sig, int tlo, int thi);

int main(int argc, char **argv) {

  MJDetInfo  Dets[NMJDETS];
  MJRunInfo  runInfo;
  int        argn=1;


  if (argc < 2) {
    fprintf(stderr, "\nusage: %s fname_in [chnum_lo] [chnum_hi] [e_lo] [e_hi] [-n]\n\n", argv[0]);
    return -1;
  }
  /* open raw data file as input */
  while (argn < argc && argv[argn][0] == '-') argn += 2;
  FILE *f_in = fopen(argv[argn],"r");
  if (f_in == NULL) {
    fprintf(stderr, "\n Failed to open input file %s\n", argv[argn]);
    return 0;
  }
  printf("\n >>> Reading %s\n\n", argv[argn]);
  strncpy(runInfo.filename, argv[argn], 256);
  runInfo.argc = argc;
  runInfo.argv = argv;

  /* read file header */
  int nDets = decode_runfile_header(f_in, Dets, &runInfo);
  if (nDets < 1) return 1;

  if (runInfo.dataIdGM == 0 && runInfo.dataIdGA == 0)
    printf("\n No data ID found for Gretina4M or 4A data!\n");
  if (runInfo.dataIdGM)
    printf("\n Data ID %d found for Gretina4M data\n", runInfo.dataIdGM);
  if (runInfo.dataIdGA)
    printf("\n Data ID %d found for Gretina4A data\n", runInfo.dataIdGA);
  printf(" Run number: %d in file %s\n", runInfo.runNumber, runInfo.filename);

/* ---------------------------------------------------- */

  int totevts=0, out_evts=0;
  int clo=0, chi, elo=3000, ehi=7200;
  int module_lu[NCRATES+1][21];  // lookup table to map VME crate&slot into module IDs
  int det_lu[NBDS][16];          // lookup table to map module&chan into detector IDs
  int chan_lu[NBDS][16];         // lookup table to map module&chan into parameter IDs
  PZinfo  PZI;
  CTCinfo CTC;
  PSAinfo PSA;

  unsigned int head[2], evtdat[20000];
  short  *signal, sigu[2048];
  float  fsignal[8192] = {0};

  // data used, stored, and reused in the different steps
  SavedData **sd;
  int    chan;
  double e_raw;
  float  drift, aovere, dcr, lamda;
  int    nsd = 0, isd = 0;  // number of saved data, and pointer to saved data id
  int    sdchunk = 1000000; // number of SavedData events to malloc at one time

  double e_ctc, e_adc;
  int    i, j, k, t90, t100;
  FILE   *f_out;


  /* initialize */

  /* malloc initial space for SavedData */
  if ((sd = malloc(sdchunk*sizeof(*sd))) == NULL ||
      (sd[0] = malloc(sdchunk*sizeof(SavedData))) == NULL) {
    printf("ERROR in skim.c; cannot malloc SavedData!\n");
    exit(-1);
  }
  for (i=1; i<sdchunk; i++) sd[i] = sd[i-1] + 1;
  nsd = sdchunk;

  if (ep_init(Dets, &runInfo, module_lu, det_lu, chan_lu) < 0) return -1;
  /* read pole-zero values from PZ.input */
  if (!PZ_info_read(&runInfo, &PZI)) {
    printf("\n ERROR: No inital pole-zero data read. Does PZ.input exist?\n");
    return -1;
  }
  /* read energy correction factors from ctc.input */
  if (!CTC_info_read(&runInfo, &CTC)) {
    printf("\n Warning: No inital charge-trapping correction data read. Does ctc.input exist?\n");
  }
  /* read individual trapezoid values from filters.input (if it exists) */
  if (2 == (PSA_info_read(&runInfo, &PSA))) {
    printf("\n Individual trap values read from filters.input\n");
  }

  // see if channel and energy limits are defined in the command line
  chi=100+runInfo.nGe-1;
  elo = 3000;
  ehi = 7200;
  j = 2;

  if (runInfo.argc > j) clo = atoi(runInfo.argv[j++]);
  if (runInfo.argc > j) chi = atoi(runInfo.argv[j++]);
  if (runInfo.argc > j) elo = atoi(runInfo.argv[j++]);
  if (runInfo.argc > j) ehi = atoi(runInfo.argv[j++]);
  if (clo < 0) clo = 0;
  if (chi > 100+runInfo.nGe) chi = 100+runInfo.nGe;

  printf("\nChs %d to %d, e_trapmax %d to %d\n\n", clo, chi, elo, ehi);

  // end of initialization
  // start loop over reading events from input file

  while (fread(head, sizeof(head), 1, f_in) == 1) {

    int board_type = head[0] >> 18;
    int evlen = (head[0] & 0x3ffff);

    if (board_type == 0) {  // a new runfile header! file must be corrupt?
      printf("\n >>>> ERROR: DataID = 0; found a second file header??"
             " Ending scan of this file!\n"
             " >>>> head = %8.8x %8.8x  evlen = %d\n", head[0], head[1], evlen);
      break;
    }

    /* if we don't want to decode this type of data, just skip forward in the file */
    if (board_type != runInfo.dataIdGM &&
        board_type != runInfo.dataIdGA) {
      if (evlen > 10000) {
        printf("\n >>>> ERROR: Event length too long??\n"
               " >>>> This file is probably corruped, ending scan!\n");
        break;
      }
      fseek(f_in, 4*(evlen-2), SEEK_CUR);
      continue;
    }

    int slot  = (head[1] >> 16) & 0x1f;
    int crate = (head[1] >> 21) & 0xf;
    if (crate < 0 || crate > NCRATES ||
        slot  < 0 || slot > 20) {
      printf("ERROR: Illegal VME crate or slot number %d %d\n", crate, slot);
      if (fread(evtdat, sizeof(int), evlen-2, f_in) != evlen-2) break;
      continue;
    }

    /* ========== read in the rest of the event data ========== */
    if (fread(evtdat, sizeof(int), evlen-2, f_in) != evlen-2) {
      printf("  No more data...\n");
      break;
    }
    if (++totevts % 50000 == 0) {
      printf(" %8d evts in, %d out, %d saved\n", totevts, out_evts, isd); fflush(stdout);
    }

    /* --------------- Gretina4M or 4A digitizer data ---------------- */
    long long int time = (evtdat[3] & 0xffff);
    time = time << 32 | evtdat[2];
    if (time < 0) continue;

    int ch = (evtdat[1] & 0xf);
    if ((j = module_lu[crate][slot]) >= 0 && ch < 10) {
      chan = chan_lu[j][ch];
      if (chan < clo || chan > chi) continue;
      signal = (short *) evtdat + 28;
      if (evlen != 1026 && signal[0] == 2020 && // signal is compressed; decompress it
          2020 == decompress_signal((unsigned short *)signal, sigu, 2*(evlen - 2) - 28)) {
        signal = sigu;
        evlen = 1026;
      }

      int e = trap_max(signal, &j, TRAP_RISE, TRAP_FLAT)/TRAP_RISE;
      if (chan < 100 && (e < elo || e > ehi)) continue;
      if (chan > 99 && (e < elo/3.4 || e > ehi/3.2)) continue;
      out_evts++;

      /* sticky-bit fix */
      int d = 128;  // basically a sensitivity threshold; max change found will be d/2
      if (chan > 99 && chan < 100+runInfo.nGe) d = 64;
      for (i=20; i<2000; i++) {
        // calculate second derivatives
        int dd0 = abs((int) signal[i+1] - 2*((int) signal[i]) + (int) signal[i-1]);
        int dd1 = abs((int) signal[i+2] - 2*((int) signal[i+1]) + (int) signal[i]);
        if (dd0 > d && dd0 > dd1 && dd0 > abs(signal[i+1] - signal[i-1])) {
          // possible occurrence; make sure it's not just high-frequency noise
          for (k=i-8; k<i+8; k++) {
            if (k==i-1 || k==i || k == i+1) continue;
            dd1 = abs((int) signal[k+1] - 2*((int) signal[k]) + (int) signal[k-1]);
            if (dd0 < dd1*3) break;
          }
          if (k<i+8) continue;
          dd0 = (int) signal[i+1] - 2*((int) signal[i]) + (int) signal[i-1];
          j = lrintf((float) dd0 / (float) d);
          printf("Fixing sticky bit in signal %d, chan %d, t=%d, change %d\n",
                 out_evts-1, chan, i, j*d/2);
          signal[i] += j*d/2;
          // break;
        }
      }

      int sig_len = 2008;

      /* ---------------------- process selected signals ---------------------- */
      
      /* find t100 and t90*/
      t100 = 700;                 // FIXME? arbitrary 700?
      for (i = t100+1; i < 1500; i++)
        if (signal[t100] < signal[i]) t100 = i;
      if (t100 > 1300) continue;  // FIXME ??  - important for cleaning, gets rid of pileup
      /* get mean baseline value */
      int bl = 0;
      for (i=300; i<400; i++) bl += signal[i];
      bl /= 100;
      if ((bl - PZI.baseline[chan]) < -10 || (bl - PZI.baseline[chan]) > 50) continue;   // a little data cleaning
      for (t90 = t100-1; t90 > 500; t90--)
        if ((signal[t90] - bl) <= (signal[t100] - bl)*19/20) break;

      /* do (optional) INL  correction */
      if (DO_INL) {
        if (inl_correct(Dets, &runInfo, signal+10, fsignal+10, sig_len-10, chan)) {
          printf(" >>> inl_correct return error for chan %d\n", chan);
          return -1;
        }
      } else {
        for (i=0; i<sig_len; i++) fsignal[i] = signal[i];
      }

      /* do fitting of pole-zero parameters to get lamda (~ DCR) */

      float chisq, lamda1, frac2;
      int tlo = t100+50, thi = 1990;  // FIXME; variable length
      // if (thi > tlo + 700) thi = tlo + 700;  // FIXME; check performance
      chisq = pz_fitter(fsignal, tlo, thi, chan, &PZI, &lamda1, &frac2, &lamda);
      if (chisq < 0.01 || chisq > 10.0) continue;            // fit failed, or bad fit chisq
      lamda = (0.01 / PZI.tau[chan] - lamda) * 1.5e6 + 0.3;  // 0.3 fudge to get mean ~ 0

      /* do (required) PZ correction */
      if (PZ_fcorrect(fsignal+10, sig_len-10, chan, &PZI))
        printf(" >>> PZ_fcorrect return error for chan %d\n", chan);

      //-------------------------

      /* get raw (e_raw) and drift-time-corrected energy (e_adc and e_ctc)
         and effective drift time */
      int tmax, t0;
      e_ctc = get_CTC_energy(fsignal, sig_len, chan, Dets, &PSA,
                             &t0, &e_adc, &e_raw, &drift, CTC.e_dt_slope[chan]);
      if (e_ctc < 0.001) {
        printf("E_ctc = %.1f < 1 eV!\n", e_ctc);
        if (VERBOSE) printf("chan, t0, t100: %d %d %d; e, drift: %.2f %.2f\n",
                            chan, t0, t100, e_raw, drift);
        continue;
      }

      if (E_THRESH > 0 && e_ctc < E_THRESH) continue;

      /* find A/E */
      if (e_ctc > 50 && t0 > 600 && t0 < 1300) {
        aovere = float_trap_max_range(fsignal, &tmax, PSA.a_e_rise[chan], 0, t0-20, t0+300);
        aovere *= PSA.a_e_factor[chan] / e_raw;
      } else {
        aovere = 0;
      }

      /* ---- This next section calculates the GERDA-style A/E ---- */
      if (PSA.gerda_aoe[chan]) {
        float  ssig[6][2000];
        for (k=100; k<2000; k++) ssig[0][k] = fsignal[k] - fsignal[300];
        for (j=1; j<6; j++) {  // number of cycles
          for (i=200; i < 1500; i++) {
            ssig[j][i] = 0.0;
            for (k=0; k<10; k++) {
              ssig[j][i] += ssig[j-1][i+k-5];
            }
            ssig[j][i] /= 10.0;
          }
        }
        aovere = 0;
        for (k=250; k<1450; k++) {
          if (aovere < ssig[5][k] - ssig[5][k-1]) aovere = ssig[5][k] - ssig[5][k-1];
        }
        aovere *= 12.71*1593/e_raw;
      }
      /* ---- end of GERDA-style A/E ---- */

      /* get DCR from slope of PZ-corrected signal tail */
      dcr = float_trap_fixed(fsignal, t90+50, 100, 500) / 25.0;

      // save data for skim file
      sd[isd]->chan     = chan;
      sd[isd]->e        = e_raw;
      sd[isd]->drift    = drift;
      sd[isd]->a_over_e = aovere;
      sd[isd]->dcr      = dcr;
      sd[isd]->lamda    = lamda;
      sd[isd]->t0       = t0;
      sd[isd]->t90      = t90;
      sd[isd]->t100     = t100;
      if (++isd == nsd) { // space allocated for saved data is now full; malloc more
        if ((sd = realloc(sd, (isd + sdchunk)*sizeof(*sd))) == NULL ||
            (sd[isd] = malloc(sdchunk*sizeof(SavedData))) == NULL) {
          printf("ERROR in skim.c; cannot realloc SavedData! nsd = %d\n", nsd);
          exit(-1);
        }
        nsd += sdchunk;
        for (i=isd+1; i<nsd; i++) sd[i] = sd[i-1]+1;
      }
    }
  }

  printf(" %8d evts in, %d out, %d saved\n", totevts, out_evts, isd);
  nsd = isd;

  // save skim data (SavedData) to disk
  f_out = fopen("skim.dat", "w");
  fwrite(&nsd, sizeof(int), 1, f_out);
  fwrite(&Dets[0], sizeof(Dets[0]), NMJDETS, f_out);
  fwrite(&runInfo, sizeof(runInfo), 1, f_out);
  // fwrite(*sd, sizeof(SavedData), nsd, f_out);
  // writing large files all in one go doesn't seem to work?
  for (isd = 0; isd < nsd; isd += sdchunk) {
    if (nsd - isd < sdchunk) {
      fwrite(sd[isd], sizeof(SavedData), nsd - isd, f_out);
    } else {
      fwrite(sd[isd], sizeof(SavedData), sdchunk, f_out);
    }
  }
  fclose(f_out);
  printf("\n Wrote %d skimmed events to skim.dat\n\n", nsd);

  fclose(f_in);
  printf("\n All Done.\n\n");
  return 0;
}