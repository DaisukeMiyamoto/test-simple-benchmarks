#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include "mpi.h"

#include "hh.h"


//#define EXP(x) hoc_Exp((x))

double hoc_Exp(double x);


static FLOAT v_trap (FLOAT x, FLOAT y) { return( (fabs(x/y) > 1e-6)? ( x/(EXP(x/y) - 1.0) ) : ( y*(1. - x/y/2.) ) ); }
static FLOAT calc_alpha_n (FLOAT v) { return( 0.01  * v_trap(-(v+55.), 10.) );   }
static FLOAT calc_beta_n  (FLOAT v) { return( 0.125 * EXP( -(v+65.) / 80.) );    }
static FLOAT calc_alpha_m (FLOAT v) { return( 0.1   * v_trap(-(v+40.), 10.));    }
static FLOAT calc_beta_m  (FLOAT v) { return( 4.    * EXP( -(v+65) / 18.) );     }
static FLOAT calc_alpha_h (FLOAT v) { return( 0.07  * EXP( -(v+65) / 20.) );     }
static FLOAT calc_beta_h  (FLOAT v) { return( 1. / (EXP( -(v+35) / 10.) + 1.) ); }


static FLOAT hh_v[N_COMPARTMENT];     // [mV]
static FLOAT hh_dv[N_COMPARTMENT];
static FLOAT hh_n[N_COMPARTMENT];
static FLOAT hh_m[N_COMPARTMENT];
static FLOAT hh_h[N_COMPARTMENT];


static FLOAT hh_cm[N_COMPARTMENT];
static FLOAT hh_cm_inv[N_COMPARTMENT];
static FLOAT hh_gk_max[N_COMPARTMENT];
static FLOAT hh_gna_max[N_COMPARTMENT];
static FLOAT hh_gm[N_COMPARTMENT];

const FLOAT hh_e_k        = -77.0;    // [mV]
const FLOAT hh_e_na       =  50.0;    // [mV]
const FLOAT hh_v_rest     = -54.3;    // [mV]

double hoc_Exp(double x){
  if (x < -700.) {
    return 0.;
  }else if (x > 700) {
    return exp(700.);
  }
  return exp(x);
}

void hh_initialize(unsigned long n_compartment)
{
  int i;
  for(i=0; i<n_compartment; i++)
    {
      hh_v[i] = -65.0;                // [mV]

      hh_n[i] = calc_alpha_n(hh_v[i]) / (calc_alpha_n(hh_v[i]) + calc_beta_n(hh_v[i]));
      hh_m[i] = calc_alpha_m(hh_v[i]) / (calc_alpha_m(hh_v[i]) + calc_beta_m(hh_v[i]));
      hh_h[i] = calc_alpha_h(hh_v[i]) / (calc_alpha_h(hh_v[i]) + calc_beta_h(hh_v[i]));

      hh_cm[i]      = 1.0;             // [muF/cm^2]
      hh_cm_inv[i]  = 1.0 / hh_cm[i];  // [cm^2/muF]
      hh_gk_max[i]  =  36.;            // [mS/cm^2]
      hh_gna_max[i] = 120.;            // [mS/cm^2] 
      hh_gm[i]      =   0.3;           // [mS/cm^3]
    }
  return;
}

#define TABLE_SIZE 201
#define TABLE_MAX_V 100.0f
#define TABLE_MIN_V -100.0f

#ifdef TABLE_TYPE
FLOAT hh_table[TABLE_SIZE][6];
#define TABLE_N_TAU(x) hh_table[(x)][0]
#define TABLE_N_INF(x) hh_table[(x)][1]
#define TABLE_M_TAU(x) hh_table[(x)][2]
#define TABLE_M_INF(x) hh_table[(x)][3]
#define TABLE_H_TAU(x) hh_table[(x)][4]
#define TABLE_H_INF(x) hh_table[(x)][5]

#else

FLOAT hh_table[6][TABLE_SIZE];
#define TABLE_N_TAU(x) hh_table[0][(x)]
#define TABLE_N_INF(x) hh_table[1][(x)]
#define TABLE_M_TAU(x) hh_table[2][(x)]
#define TABLE_M_INF(x) hh_table[3][(x)]
#define TABLE_H_TAU(x) hh_table[4][(x)]
#define TABLE_H_INF(x) hh_table[5][(x)]
#endif



void hh_makeTable()
{
  int i;
  for(i=0; i<TABLE_SIZE; i++)
    {
      FLOAT v;
      FLOAT a_n, a_m, a_h, b_n, b_m, b_h;
      v = (TABLE_MAX_V - TABLE_MIN_V)/(FLOAT)(TABLE_SIZE-1) * i + TABLE_MIN_V;
      a_n = calc_alpha_n(v);
      a_m = calc_alpha_m(v);
      a_h = calc_alpha_h(v);
      b_n = calc_beta_n(v);
      b_m = calc_beta_m(v);
      b_h = calc_beta_h(v);

      TABLE_N_TAU(i) = 1. / (a_n + b_n);
      TABLE_N_INF(i) = a_n * TABLE_N_TAU(i);
      TABLE_M_TAU(i) = 1. / (a_m + b_m);
      TABLE_M_INF(i) = a_m * TABLE_M_TAU(i);
      TABLE_H_TAU(i) = 1. / (a_h + b_h);
      TABLE_H_INF(i) = a_h * TABLE_H_TAU(i);

      //printf("%d : v(%.4f) n(%.4f %.4f) m(%.4f %.4f) h(%.4f %.4f)\n", i, v, TABLE_N_TAU(i), TABLE_N_INF(i), TABLE_M_TAU(i), TABLE_M_INF(i), TABLE_H_TAU(i), TABLE_H_INF(i) );

    }
  return;
}

void hh_calc_step(FLOAT i_inj)
{
  int j;
  unsigned int v_i_array[N_COMPARTMENT];
  FLOAT theta_array[N_COMPARTMENT];

#pragma omp parallel
  {

#ifdef KCOMPUTER
    fapp_start("prepare", 4, 2);  
#endif
    
    
#pragma omp for
    for(j=0; j<N_COMPARTMENT; j++)
      {
	v_i_array[j] = (int)(hh_v[j] - TABLE_MIN_V);
	theta_array[j] = (hh_v[j] - TABLE_MIN_V) - (FLOAT)v_i_array[j];
      }

#pragma omp for
    for(j=0; j<N_COMPARTMENT; j++)
      {
	if(!(v_i_array[j] >= TABLE_SIZE || v_i_array[j]<0) )
	  {
	    ;
	  }
	else if(v_i_array[j] >= TABLE_SIZE)
	  {
	    v_i_array[j]=TABLE_SIZE-1; theta_array[j]=1.0;
	  }
	else if(v_i_array[j] <  0)
	  {
	    v_i_array[j]=0; theta_array[j]=0.0;
	  }
      }

#ifdef KCOMPUTER
    fapp_stop("prepare", 4, 2);  
#endif
	

#ifdef KCOMPUTER
    fapp_start("state", 2, 2);  
#endif


#pragma omp for
    for(j=0; j<N_COMPARTMENT; j++)
      {
	FLOAT tau_n, n_inf, tau_m, m_inf, tau_h, h_inf;
	unsigned int v_i = v_i_array[j];
	FLOAT theta = theta_array[j];
	
	tau_n = TABLE_N_TAU(v_i);
	n_inf = TABLE_N_INF(v_i);
	tau_m = TABLE_M_TAU(v_i);
	m_inf = TABLE_M_INF(v_i);
	tau_h = TABLE_H_TAU(v_i);
	h_inf = TABLE_H_INF(v_i);
	
	tau_n = (tau_n + theta * (TABLE_N_TAU(v_i+1) - tau_n));
	tau_m = (tau_m + theta * (TABLE_M_TAU(v_i+1) - tau_m));
	tau_h = (tau_h + theta * (TABLE_H_TAU(v_i+1) - tau_h));
	n_inf = n_inf + theta * (TABLE_N_INF(v_i+1) - n_inf) - hh_n[j];
	m_inf = m_inf + theta * (TABLE_M_INF(v_i+1) - m_inf) - hh_m[j];
	h_inf = h_inf + theta * (TABLE_H_INF(v_i+1) - h_inf) - hh_h[j];
	
	hh_n[j] += (1.0f - EXP(-DT/tau_n)) * n_inf;
	hh_m[j] += (1.0f - EXP(-DT/tau_m)) * m_inf;
	hh_h[j] += (1.0f - EXP(-DT/tau_h)) * h_inf;
	
	
	hh_v[j] += DT * hh_cm_inv[0] * (hh_gk_max[0]  * hh_n[j] * hh_n[j] * hh_n[j] * hh_n[j] * (hh_e_k - hh_v[j]) 
					+ hh_gna_max[0] * hh_m[j] * hh_m[j] * hh_m[j] * hh_h[j] * (hh_e_na - hh_v[j])
					+ hh_gm[0] * (hh_v_rest - hh_v[j]) + i_inj);
      }
    
#ifdef KCOMPUTER
    fapp_stop("state", 2, 2);  
#endif

  }
}

int hh_calc(FLOAT stoptime)
{
  unsigned int i;
  unsigned int i_stop;

  const int inj_start =  50./DT;
  const int inj_stop  = 300./DT;

  for(i=0,i_stop=stoptime/DT; i<i_stop; i++)
    {
      FLOAT i_inj;
      if(i > inj_start && i < inj_stop)
	{
	  i_inj = 10.0;
	}
      else
	{
	  i_inj = 0.0;
	}
      //printf("%f %f %f %f\n", i*DT, i_inj, hh_v[0], hh_v[N_COMPARTMENT-1]);

      hh_calc_step(i_inj);
      printf ("%f, %f, %f\n", i*DT, i_inj, hh_v[0]);
    }
  return(0);
}


int main(int argc, char **argv)
{
  int myid;
  MPI_Init(&argc, &argv);
  MPI_Comm_rank(MPI_COMM_WORLD, &myid);

  hh_initialize(N_COMPARTMENT);
  hh_makeTable();

#ifdef KCOMPUTER
  fapp_start("calc", 1, 1);  
#endif

  //printf("start (%d)\n", myid);
  printf ("# Hodgkin-Huxley Benchmark for OpenCL\n");
  printf ("# nebula (20151010)\n");
  printf ("# t , i_inj [nA], V [mV]\n");
  hh_calc(400);
  //printf("finished (%d)\n", myid);

#ifdef KCOMPUTER
  fapp_stop("calc", 1, 1);
#endif

  MPI_Finalize();
  return(0);

}