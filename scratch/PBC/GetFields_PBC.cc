/* Copyright (C) 2005-2011 M. T. Homer Reid
 *
 * This file is part of SCUFF-EM.
 *
 * SCUFF-EM is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * SCUFF-EM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
 * GetFields_PBC.cc  
 *
 * homer reid        -- 7/2011 -- 7/2012
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#include <libhrutil.h>
#include <libTriInt.h>

#include "libscuff.h"
#include "FieldGrid.h"

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif
#ifdef USE_PTHREAD
#  include <pthread.h>
#endif

#define MAXFUNC 50

namespace scuff {

#define II cdouble(0,1)

/*******************************************************************/
/* 'GetReducedPotentialsIntegrand' is the integrand routine passed */
/* to TriInt to compute the reduced potentials of a single panel.  */
/*                                                                 */
/* integrand values:                                               */
/*  f[0],  f[1]  == real , imag  (X-Q)_x            Phi(|X-X0|)    */
/*  f[2],  f[3]  == real , imag  (X-Q)_y            Phi(|X-X0|)    */
/*  f[4],  f[5]  == real , imag  (X-Q)_z            Phi(|X-X0|)    */
/*  f[6],  f[7]  == real , imag  [(X-Q) x (X-X0)]_x Psi(|X-X0|)    */
/*  f[8],  f[9]  == real , imag  [(X-Q) x (X-X0)]_y Psi(|X-X0|)    */
/*  f[10], f[11] == real , imag  [(X-Q) x (X-X0)]_z Psi(|X-X0|)    */
/*  f[12], f[13] == real , imag  2(X-X0)_x          Psi(|X-X0|)    */
/*  f[14], f[15] == real , imag  2(X-X0)_y          Psi(|X-X0|)    */
/*  f[16], f[17] == real , imag  2(X-X0)_z          Psi(|X-X0|)    */
/*******************************************************************/
typedef struct GRPIntegrandData
 { 
   double *Q;         // RWG basis function source/sink vertex  
   double PreFac;     // RWG basis function prefactor 
   const double *X0;  // field evaluation point 
   cdouble K;         // \sqrt{Eps*Mu} * frequency
   double *P;         // bloch wavevector 
   double **LBV;      // lattice basis vectors
 } GRPIData;

static void GRPIntegrand(double *X, void *parms, double *f)
{ 
  GRPIntegrandData *GRPID=(GRPIntegrandData *)parms;
  double *Q         = GRPID->Q;
  double PreFac     = GRPID->PreFac;
  const double *X0  = GRPID->X0;
  cdouble K         = GRPID->K;
  double *P         = GRPID->P;
  double **LBV      = GRPID->LBV;

  /* get the value of the RWG basis function at X */
  double fRWG[3];
  VecSub(X,Q,fRWG);
  VecScale(fRWG,PreFac);
  
  /* compute the periodic green's function using ewald summation */
  cdouble GBarVD[8];
  double XmX0[3];
  VecSub(X,X0,XmX0);
  GBarVDEwald(XmX0, K, P, LBV, -1.0, 0, GBarVD);
  cdouble Phi = GBarVD[0], *GradPhi=GBarVD+1;
  
  /* assemble integrand components */
  cdouble *zf=(cdouble *)f;
  zf[0]= fRWG[0] * Phi;
  zf[1]= fRWG[1] * Phi;
  zf[2]= fRWG[2] * Phi;
  zf[3]= (fRWG[1] * GradPhi[2] - fRWG[2] * GradPhi[1]);
  zf[4]= (fRWG[2] * GradPhi[0] - fRWG[0] * GradPhi[2]);
  zf[5]= (fRWG[0] * GradPhi[1] - fRWG[1] * GradPhi[0]);
  zf[6]= -2.0 * PreFac * GradPhi[0];
  zf[7]= -2.0 * PreFac * GradPhi[1];
  zf[8]= -2.0 * PreFac * GradPhi[2];

} 

/***************************************************************/
/* Compute the 'reduced potentials' of a single RWG basis      */
/* function.                                                   */
/*                                                             */
/* The 'reduced potentials' are dimensionless analogues of the */
/* scalar and vector potentials of the source distributions    */
/* described by the RWG function populated with unit strength: */
/*                                                             */
/*    p(x) = \int G(x,y) \nabla \cdot f(y) dy                  */
/*  a_i(x) = \int G(x,y) f_i(y) dy                             */
/*                                                             */
/***************************************************************/
void GetReducedPotentials(RWGObject *O, int ne, const double *X, 
                          cdouble K, double *P, double **LBV,
                          cdouble *a, cdouble *Curla, cdouble *Gradp)
{
  double *QP, *V1, *V2, *QM;
  double PArea, MArea;
  RWGEdge *E;
  int mu;
  GRPIntegrandData MyGRPIData, *GRPID=&MyGRPIData;
  cdouble IP[9], IM[9];

  /* get edge vertices */
  E=Edges[ne];
  QP=Vertices + 3*(E->iQP);
  V1=Vertices + 3*(E->iV1);
  V2=Vertices + 3*(E->iV2);
  QM=Vertices + 3*(E->iQM);
  PArea=Panels[E->iPPanel]->Area;
  MArea=Panels[E->iMPanel]->Area;

  /* set up data structure passed to GRPIntegrand */
  GRPID->X0=X;
  GRPID->K=K;
  GRPID->P=P;
  GRPID->LBV=LBV;

  /* contribution of positive panel */
  GRPID->Q=QP;
  GRPID->PreFac = E->Length / (2.0*PArea);
  TriIntFixed(GRPIntegrand, 18, (void *)GRPID, QP, V1, V2, 25, (double *)IP);

  /* contribution of negative panel */
  GRPID->Q=QM;
  GRPID->PreFac = E->Length / (2.0*MArea);
  TriIntFixed(GRPIntegrand, 18, (void *)GRPID, V1, V2, QM, 25, (double *)IM);

  for(mu=0; mu<3; mu++) 
   { a[mu]     = IP[mu]   - IM[mu];
     Curla[mu] = IP[mu+3] - IM[mu+3];
     Gradp[mu] = IP[mu+6] - IM[mu+6];
   };

}

/***************************************************************/
/***************************************************************/
/***************************************************************/
void GetScatteredFields(PBCGeometry *PG, 
                        const double *X, 
                        const int ObjectIndex, 
                        HVector *KN,
                        const cdouble Omega, 
                        const double *P,
                        const cdouble Eps,
                        const cdouble Mu,
                        cdouble EHS[6])
{ 
  memset(EHS, 0, 6*sizeof(cdouble));

  cdouble iwe=II*Omega*Eps;
  cdouble iwu=II*Omega*Mu;
  cdouble K=csqrt2(Eps*Mu)*Omega;

  RWGGeometry *G = PG->G;
  RWGObject *O;
  int i, ne, no, Type, Offset;
  cdouble KAlpha, NAlpha, a[3], Curla[3], Gradp[3];
  double Sign;
  int ContainingObjectIndex;
  for(no=0; no<G->NumObjects; no++)
   { 
     O=G->Objects[no];

     Type=O->MP->Type;
     Offset=G->BFIndexOffset[O->Index];
     if (O->ContainingObject==NULL)
      ContainingObjectIndex=-1;
     else
      ContainingObjectIndex=O->ContainingObject->Index;

     /******************************************************************/
     /* figure out the sign of the contribution of currents on this    */
     /* object's surface to the field at the evaluation point.         */
     /*****************************************************************/
     if ( ObjectIndex==O->Index )
      Sign=-1.0;
     else if ( ObjectIndex==ContainingObjectIndex )
      Sign=+1.0;
     else
      continue; // in this case O does not contribute to field at eval pt

     /***************************************************************/
     /* now loop over panels on object's surface to get             */
     /* contributions to field at evaluation point.                 */
     /***************************************************************/
     for(ne=0; ne<O->NumEdges; ne++)
      { 
        if ( Type==MP_PEC )
         { 
           KAlpha = Sign*KN->GetEntry( Offset + ne );
         }
        else
         { KAlpha = Sign*KN->GetEntry( Offset + 2*ne + 0 );
           NAlpha = Sign*KN->GetEntry( Offset + 2*ne + 1 );
         };
      
        GetReducedPotentials(O, ne, X, K, P, PG->LBV, a, Curla, Gradp);

        for(i=0; i<3; i++)
         { EHS[i]   += ZVAC*( KAlpha*(iwu*a[i] - Gradp[i]/iwe) + NAlpha*Curla[i] );
           EHS[i+3] += -1.0*NAlpha*(iwe*a[i] - Gradp[i]/iwu) + KAlpha*Curla[i];
         };

      }; // for (ne=0 ... 

    }; // for(no=0 ... 
}


/***************************************************************/
/***************************************************************/
/***************************************************************/
typedef struct ThreadData
 { 
   int nt, nTask;

   PBCGeometry *PG;
   HMatrix *XMatrix;
   HMatrix *FMatrix;
   HVector *KN;
   IncField *IF;
   cdouble Omega;
   double *P;
   ParsedFieldFunc **PFFuncs;
   int NumFuncs;

 } ThreadData;

/***************************************************************/
/***************************************************************/
/***************************************************************/
void *GetFields_Thread(void *data)
{
  ThreadData *TD=(ThreadData *)data;

#ifdef USE_PTHREAD
  SetCPUAffinity(TD->nt);
#endif

  /***************************************************************/
  /* fields unpacked from thread data structure ******************/
  /***************************************************************/
  PBCGeometry *PG             = TD->PG;
  RWGGeometry *G              = PG->G;
  HMatrix *XMatrix            = TD->XMatrix;
  HMatrix *FMatrix            = TD->FMatrix;
  HVector *KN                 = TD->KN;
  IncField *IFList            = TD->IF;
  cdouble Omega               = TD->Omega;
  double *P                   = TD->P;
  ParsedFieldFunc **PFFuncs   = TD->PFFuncs;
  int NumFuncs                = TD->NumFuncs;

  /***************************************************************/
  /* other local variables ***************************************/
  /***************************************************************/
  double X[3];
  int ObjectIndex;
  cdouble EH[6], dEH[6];
  cdouble Eps, Mu;
  double dA[3]={0.0, 0.0, 0.0};
  IncField *IF;

  /***************************************************************/
  /* loop over all eval points (all rows of the XMatrix)         */
  /***************************************************************/
  int nt=0;
  for(int nr=0; nr<XMatrix->NR; nr++)
   { 
     nt++;
     if (nt==TD->nTask) nt=0;
     if (nt!=TD->nt) continue;

     X[0]=XMatrix->GetEntryD(nr, 0);
     X[1]=XMatrix->GetEntryD(nr, 1);
     X[2]=XMatrix->GetEntryD(nr, 2);

// FIXME 
#if 0
     ObjectIndex = G->GetObjectIndex(X);
#endif
     ObjectIndex = -1;

     if (ObjectIndex==-1)
      G->ExteriorMP->GetEpsMu(Omega, &Eps, &Mu);
     else
      G->Objects[ObjectIndex]->MP->GetEpsMu(Omega, &Eps, &Mu);
    
     /*--------------------------------------------------------------*/
     /*- get scattered fields at X                                   */
     /*--------------------------------------------------------------*/
     if (KN)
      GetScatteredFields(PG, X, ObjectIndex, KN, Omega, P, Eps, Mu, EH);
     else
      memset(EH, 0, 6*sizeof(cdouble));

     /*--------------------------------------------------------------*/
     /*- add incident fields by summing contributions of all        -*/
     /*- IncFields whose sources lie in the same region as X        -*/
     /*--------------------------------------------------------------*/
     if (IFList)
      { for(IF=IFList; IF; IF=IF->Next)
         if ( IF->ObjectIndex == ObjectIndex )
          { IF->GetFields(X, dEH);
            SixVecPlusEquals(EH, 1.0, dEH);
          };
      };

     /*--------------------------------------------------------------*/
     /*- compute field functions ------------------------------------*/
     /*--------------------------------------------------------------*/
     for(int nf=0; nf<NumFuncs; nf++)
      FMatrix->SetEntry(nr, nf, PFFuncs[nf]->Eval(X, dA, EH, Eps, Mu));

   }; // for (nr=0; nr<XMatrix->NR; nr++)

  return 0;

} 

/***************************************************************/
/***************************************************************/
/***************************************************************/
HMatrix *PBCGeometry::GetFields(IncField *IF, HVector *KN,
                                cdouble Omega, double *P, HMatrix *XMatrix,
                                HMatrix *FMatrix, char *FuncString,
                                int nThread)
{ 
  if (nThread <= 0) nThread = GetNumThreads();
 
  /***************************************************************/
  /* preprocess the Functions string to count the number of      */
  /* comma-separated function strings and verify that each string*/
  /* is a valid function                                         */
  /***************************************************************/
  char *FCopy;
  char *Funcs[MAXFUNC];
  int nf, NumFuncs;

  if (FuncString==NULL)
   FCopy=strdup("Ex,Ey,Ez,Hx,Hy,Hz"); // default is cartesian field components
  else
   FCopy=strdup(FuncString);

  NumFuncs=Tokenize(FCopy, Funcs, MAXFUNC, ",");

  ParsedFieldFunc **PFFuncs = new ParsedFieldFunc *[NumFuncs];
  for(nf=0; nf<NumFuncs; nf++)
   PFFuncs[nf] = new ParsedFieldFunc(Funcs[nf]);

  /***************************************************************/
  /***************************************************************/
  /***************************************************************/
  if ( XMatrix==0 || XMatrix->NC!=3 || XMatrix->NR==0 )
   ErrExit("wrong-size XMatrix (%ix%i) passed to GetFields",XMatrix->NR,XMatrix->NC);

  if (FMatrix==0) 
   FMatrix=new HMatrix(XMatrix->NR, NumFuncs, LHM_COMPLEX);
  else if ( (FMatrix->NR != XMatrix->NR) || (FMatrix->NC!=NumFuncs) ) 
   { Warn(" ** warning: wrong-size FMatrix passed to GetFields(); allocating new matrix");
     FMatrix=new HMatrix(XMatrix->NR, NumFuncs, LHM_COMPLEX);
   };

  /***************************************************************/
  /* the incident fields will most likely have been updated at   */
  /* the current frequency already by an earlier call to         */
  /* AssembleRHSVector(), but somewhat might call GetFields()    */
  /* to get information on just the incident fields before       */
  /* before setting up and solving the BEM problem, so we should */
  /* do this just to make sure.                                  */
  /***************************************************************/
  G->UpdateIncFields(IF, Omega);

  /***************************************************************/
  /* fire off threads                                            */
  /***************************************************************/
  int nt;

  // set up an instance of ThreadData containing all fields
  // that are common to all threads, which we can subsequently
  // copy wholesale to initialize new ThreadData structures
  ThreadData ReferenceTD; 
  ReferenceTD.PG=this;
  ReferenceTD.XMatrix = XMatrix;
  ReferenceTD.FMatrix = FMatrix;
  ReferenceTD.KN=KN;
  ReferenceTD.IF=IF;
  ReferenceTD.Omega=Omega;
  ReferenceTD.P=P;
  ReferenceTD.PFFuncs=PFFuncs;
  ReferenceTD.NumFuncs=NumFuncs;

#ifdef USE_PTHREAD
  ThreadData *TDs = new ThreadData[nThread], *TD;
  pthread_t *Threads = new pthread_t[nThread];
  ReferenceTD.nTask=nThread;
  for(nt=0; nt<nThread; nt++)
   { 
     TD=&(TDs[nt]);
     memcpy(TD, &ReferenceTD, sizeof(ThreadData));
     TD->nt=nt;

     if (nt+1 == nThread)
       GetFields_Thread((void *)TD);
     else
       pthread_create( &(Threads[nt]), 0, GetFields_Thread, (void *)TD);
   }
  for(nt=0; nt<nThread-1; nt++)
   pthread_join(Threads[nt],0);

  delete[] Threads;
  delete[] TDs;

#else 
#ifndef USE_OPENMP
  nThread=ReferenceTD.nTask=1;
#else
  ReferenceTD.nTask=nThread*100;
#pragma omp parallel for schedule(dynamic,1), num_threads(nThread)
#endif
  for(nt=0; nt<ReferenceTD.nTask; nt++)
   { 
     ThreadData TD1;
     memcpy(&TD1, &ReferenceTD, sizeof(ThreadData));
     TD1.nt=nt;
     GetFields_Thread((void *)&TD1);
   };
#endif

  free(FCopy);
  for(nf=0; nf<NumFuncs; nf++)
   delete PFFuncs[nf];
  delete[] PFFuncs;

  return FMatrix;

}

/***************************************************************/
/***************************************************************/
/***************************************************************/
void PBCGeometry::GetFields(IncField *IF, HVector *KN, 
                            cdouble Omega, double *P, 
                            double *X, cdouble *EH, int nThread)
{
  HMatrix XMatrix(1, 3, LHM_REAL, LHM_NORMAL, (void *)X);

  HMatrix FMatrix(1, 6, LHM_COMPLEX, LHM_NORMAL, (void *)EH);

  GetFields(IF, KN, Omega, P, &XMatrix, &FMatrix, 0, nThread);
} 


} // namespace scuff
