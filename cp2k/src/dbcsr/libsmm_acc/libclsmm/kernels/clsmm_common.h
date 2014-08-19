/*****************************************************************************
 *  CP2K: A general program to perform molecular dynamics simulations        *
 *  Copyright (C) 2000 - 2013 the CP2K developers group                      *
 *****************************************************************************/
#ifndef CLSMM_COMMON_H
#define CLSMM_COMMON_H

#if defined (__ACC)

#if defined (cl_khr_fp64)    // NVIDIA, Intel, Khronos
#pragma OPENCL EXTENSION cl_khr_fp64 : enable
#elif defined (cl_amd_fp64)  // AMD
#pragma OPENCL EXTENSION cl_amd_fp64 : enable
#endif

#if defined (cl_intel_printf)    // Intel
#pragma OPENCL EXTENSION cl_intel_printf : enable
#elif defined (cl_amd_printf)    // AMD
#pragma OPENCL EXTENSION cl_amd_printf : enable
#endif

#if defined (cl_khr_int64_base_atomics) // native 64-bit atomics support
#pragma OPENCL EXTENSION cl_khr_int64_base_atomics : enable
/******************************************************************************
 * There is no nativ support for atomicAdd on doubles in OpenCL 1.1.          *
 ******************************************************************************/
inline void AtomicAdd (__global volatile double       *address,
                                         const double val)
{
  union {
    unsigned long intVal;
    double dblVal;
  } newVal;
  union {
    unsigned long intVal;
    double dblVal;
  } prevVal;
  do {
    prevVal.dblVal = *address;
    newVal.dblVal = prevVal.dblVal + val;
  } while (atomic_cmpxchg((volatile __global unsigned long *) address, prevVal.intVal, newVal.intVal) != prevVal.intVal);
}

#else // build-in 32-bit atomics support
inline void AtomicAdd (__global volatile double       *address,
                                         const double val)
{
  __global volatile unsigned int *ahi, *alo;
  __private         unsigned int *phi, *plo, *nhi, *nlo;
  __private         bool         failure;
  failure = true;
  while (failure) {
    ahi = (__global volatile unsigned int *) ((char *) address + 0);
    alo = (__global volatile unsigned int *) ((char *) address + 4);
    __private double p = *address;
    phi = (__private unsigned int *) ((char *) &p + 0);
    plo = (__private unsigned int *) ((char *) &p + 4);
    __private double n = p + val;
    nhi = (__private unsigned int *) ((char *) &n + 0);
    nlo = (__private unsigned int *) ((char *) &n + 4);
    if (atom_cmpxchg((__global volatile unsigned int *) ahi, *phi, *nhi) == *phi) {
      while (failure) {   
        if (atomic_cmpxchg((__global volatile unsigned int *) alo, *plo, *nlo) == *plo) {
          failure = false;
        }
      }
    }
  }
}
#endif

//**************************************************************************//
inline void load_gmem_into_smem(__global double    *from,
                                __local  double    *dest,
                                         const int length,
                                         const int blockdim)
{
    if (length < blockdim) { // are there enough threads to load in one step?
        if (get_local_id(0) < length)
            //dest[get_local_id(0)] = __ldg(from + get_local_id(0));
            dest[get_local_id(0)] = *(from + get_local_id(0));
    } else {
        for (int i = get_local_id(0); i < length; i += blockdim)
            //dest[i] = __ldg(from + i);
            dest[i] = *(from + i);
    }
}


//**************************************************************************//
inline void load_gmem_into_regs(__global  double    *from,
                                __private double    *dest,
                                          const int length,
                                          const int blockdim)
{
    const int NR = (length + blockdim - 1) / blockdim;

    if (length < blockdim) { // are there enough threads to load in one step?
        if (get_local_id(0) < length)
            //dest[0] = __ldg(from + get_local_id(0));
            dest[0] = *(from + get_local_id(0));
    } else {
        int i = get_local_id(0);
        for (int ri = 0; ri < NR; ri++) {  //loop with fixed bounds
            if (i < length)
                //dest[ri] = __ldg(from + i);
                dest[ri] = *(from + i);
            i += blockdim;
        }
    }
}


//**************************************************************************//
inline void load_regs_into_smem(__private double    *from,
                                __local   double    *dest,
                                          const int length,
                                          const int blockdim)
{
   const int NR = (length + blockdim - 1) / blockdim;

   if (length < blockdim) { // are there enough threads to load in one step?
       if (get_local_id(0) < length)
           dest[get_local_id(0)] = from[0];
   } else {
        int i = get_local_id(0);
        for (int ri = 0; ri < NR; ri++) {  //loop with fixed bounds
            if (i < length)
                dest[i] = from[ri];
            i += blockdim;
        }
    }
}


//**************************************************************************//
inline void multiply(__local   double    *buff_a,
                     __local   double    *buff_b,
                     __private double    *buff_c,
                               const int w,
                               const int m,
                               const int n,
                               const int M,
                               const int N,
                               const int blockdim)
{
    // There might be more threads than needed for the calculation.
    // Only the first cmax*rmax threads participate in the calculation.

    const int cmax = (n + N - 1) / N; // max tile-column
    const int rmax = (m + M - 1) / M; //  max tile-row
    const int c = get_local_id(0) / rmax; // this thread's tile-column
    const int r = get_local_id(0) - c * rmax; // this thread's tile-row

    if (c < cmax && r < rmax) // is this thread participating?
        for (int l = 0; l < w; l++)
            for (int i = 0; i < N; i++)
                for (int j = 0; j < M; j++)
                    buff_c[M * i + j] +=
                        buff_a[l * m + M * r + j] * buff_b[l * n + N * c + i];
}


//**************************************************************************//
inline void store_results_into_smem(__private double *from,
                                    __local   double *dest,
                                              const int t,
                                              const int v,
                                              const int m,
                                              const int n,
                                              const int M,
                                              const int N,
                                              const int blockdim)
{
    const int rmax = (m + M - 1) / M; //  max tile-row
    const int c = get_local_id(0) / rmax; // this thread's tile-column
    const int r = get_local_id(0) - c * rmax; // this thread's tile-row

    int ctmp = c * N - t;
    if (ctmp >= -(N - 1) && ctmp < v)
        for (int i = 0; i < N; i++)
            if (ctmp + i >= 0 && ctmp + i < v)
                for (int j = 0; j < M; j++)
                    if (M * r + j < m) {
                        dest[(ctmp + i) * m + M * r + j] = from[M * i + j];
                        from[M * i + j] = 0.0; // reset result tile
                    }

}

#endif
#endif
