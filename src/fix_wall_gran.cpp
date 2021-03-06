/* ----------------------------------------------------------------------
LIGGGHTS - LAMMPS Improved for General Granular and Granular Heat
Transfer Simulations

www.liggghts.com | www.cfdem.com
Christoph Kloss, christoph.kloss@cfdem.com

LIGGGHTS is based on LAMMPS
LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
http://lammps.sandia.gov, Sandia National Laboratories
Steve Plimpton, sjplimp@sandia.gov

Copyright (2003) Sandia Corporation. Under the terms of Contract
DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
certain rights in this software. This software is distributed under
the GNU General Public License.

See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

/* ----------------------------------------------------------------------
   Contributing authors for original version: Leo Silbert (SNL), Gary Grest (SNL)
------------------------------------------------------------------------- */

#include "math.h"
#include "stdlib.h"
#include "string.h"
#include "fix_wall_gran.h"
#include "atom.h"
#include "domain.h"
#include "update.h"
#include "force.h"
#include "pair_gran.h"
#include "fix_rigid.h"
#include "modify.h"
#include "respa.h"
#include "memory.h"
#include "fix_meshGran.h"
#include "comm.h"
#include "error.h"
#include "fix_propertyPerAtom.h"
#include "fix_propertyGlobal.h"
#include "math_extra.h"
#include "triSpherePenetration.h"
#include "fix_tri_neighlist.h"

using namespace LAMMPS_NS;

#define BIG 1.0e20

#define MIN(A,B) ((A) < (B)) ? (A) : (B)
#define MAX(A,B) ((A) > (B)) ? (A) : (B)

#define DELTA_TRI_CONTACTS 6 

#define EPSILON_MOVINGMESH 1e-10
#define SMALL 1e-8

/* ---------------------------------------------------------------------- */

FixWallGran::FixWallGran(LAMMPS *lmp, int narg, char **arg) :
  Fix(lmp, narg, arg)
{
  if(strcmp(style,"wall/gran") == 0)
      error->all("Fix wall/gran is deprecated in LIGGGHTS. Please use fix wall/gran/hooke, "
                 "fix wall/gran/hooke/history, fix wall/gran/hertz/history etc. according to the pair style you use");

  if(strncmp(style,"wall/gran/",10)) error->all("Implementation requires styles for granular wall have to start with wall/gran/...");

  if (narg < 7) error->all("Illegal fix wall/gran command");

  if (!atom->radius_flag || !atom->omega_flag || !atom->torque_flag)
    error->all("Fix wall/gran requires atom attributes radius, omega, torque");

  FixMeshGranList = NULL;
  fix_tri_neighlist = NULL;
  meshwall = 0;
  Temp_wall = -1.;

  fppa_T = NULL;
  fppa_hf = NULL;
  Temp_p = NULL;
  heatflux = NULL;

  wiggle = 0;
  wshear = 0;
  lo = hi = 0.0;

  int iarg = 5;
  if (strncmp(arg[iarg],"mesh/gran",8) == 0) {
      wallstyle = MESHGRAN_FWG;
      meshwall = 1;

      for (int ifix=0;ifix<modify->nfix;ifix++)
      {
          if(strncmp(modify->fix[ifix]->style,"wall/gran/",9)==0)
          {
              FixWallGran* othF = static_cast<FixWallGran*>(modify->fix[ifix]);
              if (othF->meshwall) error->all("There must be only one fix wall/gran with style 'mesh/gran'");
          }
      }

      if (narg < iarg+2) error->all("Illegal fix wall/gran command, not enough arguments");
      nFixMeshGran = atoi(arg[iarg+1]);
      if (narg < iarg+2+nFixMeshGran) error->all("Illegal fix wall/gran command, not enough arguments");
      FixMeshGranList=new FixMeshGran*[nFixMeshGran];
      for (int i=0;i<nFixMeshGran;i++) FixMeshGranList[i]=static_cast<FixMeshGran*>(NULL);
      for(int i=0;i<nFixMeshGran;i++)
      {
          int f_i=modify->find_fix(arg[iarg+2+i]);
          if (f_i==-1) error->all("Could not find fix mesh/gran id you provided for the fix wall/gran command");
          if (strncmp(modify->fix[f_i]->style,"mesh/gran",8) != 0) error->all("fix wall/gran: A fix belonging to the id you provided is not of type mesh/gran");
          FixMeshGranList[i]=static_cast<FixMeshGran*>(modify->fix[f_i]);
      }

      iarg += (2+nFixMeshGran);
  }else if (strcmp(arg[iarg],"xplane") == 0) {
    if (narg < iarg+4) error->all("Illegal fix wall/gran command");
    wallstyle = XPLANE_FWG;
    if (strcmp(arg[iarg+1],"NULL") == 0) lo = -BIG;
    else lo = atof(arg[iarg+1]);
    if (strcmp(arg[iarg+2],"NULL") == 0) hi = BIG;
    else hi = atof(arg[iarg+2]);
    atom_type_wall=atoi(arg[iarg+3]);
    iarg += 4;
  } else if (strcmp(arg[iarg],"yplane") == 0) {
    if (narg < iarg+3) error->all("Illegal fix wall/gran command");
    wallstyle = YPLANE_FWG;
    if (strcmp(arg[iarg+1],"NULL") == 0) lo = -BIG;
    else lo = atof(arg[iarg+1]);
    if (strcmp(arg[iarg+2],"NULL") == 0) hi = BIG;
    else hi = atof(arg[iarg+2]);
    atom_type_wall=atoi(arg[iarg+3]);
    iarg += 4;
  } else if (strcmp(arg[iarg],"zplane") == 0) {
    if (narg < iarg+3) error->all("Illegal fix wall/gran command");
    wallstyle = ZPLANE_FWG;
    if (strcmp(arg[iarg+1],"NULL") == 0) lo = -BIG;
    else lo = atof(arg[iarg+1]);
    if (strcmp(arg[iarg+2],"NULL") == 0) hi = BIG;
    else hi = atof(arg[iarg+2]);
    atom_type_wall=atoi(arg[iarg+3]);
    iarg += 4;
  } else if (strcmp(arg[iarg],"zcylinder") == 0) {
    if (narg < iarg+3) error->all("Illegal fix wall/gran command");
    wallstyle = ZCYLINDER_FWG;
    lo = hi = 0.0;
    cylradius = atof(arg[iarg+1]);
    atom_type_wall=atoi(arg[iarg+2]);
    iarg += 3;
  }

  if(lo > hi) error->all("Fix wall/gran: lo value of wall position must be < hi value");

  // check for trailing keyword/values

  while (iarg < narg) {
    if (strcmp(arg[iarg],"wiggle") == 0) {
      if (wallstyle==MESHGRAN_FWG) error->all("Fix wall/gran: Can not use wiggle together with style mesh/gran. Please use fix move/mesh/gran for moving mesh");
      if (iarg+4 > narg) error->all("Illegal fix wall/gran command");
      if (strcmp(arg[iarg+1],"x") == 0) axis = 0;
      else if (strcmp(arg[iarg+1],"y") == 0) axis = 1;
      else if (strcmp(arg[iarg+1],"z") == 0) axis = 2;
      else error->all("Illegal fix wall/gran command");
      amplitude = atof(arg[iarg+2]);
      period = atof(arg[iarg+3]);
      wiggle = 1;
      iarg += 4;
    } else if (strcmp(arg[iarg],"shear") == 0) {
      if (wallstyle==MESHGRAN_FWG) error->all("Fix wall/gran: Can not use shear together with style mesh/gran. Please use fix move/mesh/gran for moving mesh");
      if (iarg+3 > narg) error->all("Illegal fix wall/gran command");
      if (strcmp(arg[iarg+1],"x") == 0) axis = 0;
      else if (strcmp(arg[iarg+1],"y") == 0) axis = 1;
      else if (strcmp(arg[iarg+1],"z") == 0) axis = 2;
      else error->all("Illegal fix wall/gran command");
      vshear = atof(arg[iarg+2]);
      wshear = 1;
      iarg += 3;
    } else if (strcmp(arg[iarg],"temperature") == 0) {
        iarg++;
        Temp_wall = atof(arg[iarg++]);
    }else error->all("Illegal fix wall/gran command");
  }

  if (wallstyle == XPLANE_FWG && domain->xperiodic)
    error->all("Cannot use wall in periodic dimension");
  if (wallstyle == YPLANE_FWG && domain->yperiodic)
    error->all("Cannot use wall in periodic dimension");
  if (wallstyle == ZPLANE_FWG && domain->zperiodic)
    error->all("Cannot use wall in periodic dimension");
  if (wallstyle == ZCYLINDER_FWG && (domain->xperiodic || domain->yperiodic))
    error->all("Cannot use wall in periodic dimension");

  if (wiggle && wshear) error->all("Cannot wiggle and shear fix wall/gran");
  if (wiggle && wallstyle == ZCYLINDER_FWG && axis != 2)
    error->all("Invalid wiggle direction for fix wall/gran");
  if (wshear && wallstyle == XPLANE_FWG && axis == 0)
    error->all("Invalid shear direction for fix wall/gran");
  if (wshear && wallstyle == YPLANE_FWG && axis == 1)
    error->all("Invalid shear direction for fix wall/gran");
  if (wshear && wallstyle == ZPLANE_FWG && axis == 2)
    error->all("Invalid shear direction for fix wall/gran");

  // setup oscillations

  if (wiggle) {
    double PI = 4.0 * atan(1.0);
    omega = 2.0*PI / period;
  }

  restart_global = 1;
  restart_peratom = 1;
  create_attribute = 1;

  // perform initial allocation of atom-based arrays
  // register with Atom class

  npartners = NULL;
  partner = NULL;
  contacthistory = NULL;
  dnum = 0;

  if (meshwall) maxpartners = DELTA_TRI_CONTACTS;
  else maxpartners=1;

  atom->add_callback(0);
  atom->add_callback(1);

  time_depend = 1;

  time_origin = update->ntimestep;

  pairgran = NULL;

  laststep = -1;
}

/* ---------------------------------------------------------------------- */

FixWallGran::~FixWallGran()
{
  // unregister callbacks to this fix from Atom class
  atom->delete_callback(id,0);
  atom->delete_callback(id,1);

  // delete locally stored arrays
  memory->destroy_3d_double_array(contacthistory);
  memory->destroy_3d_int_array(partner);
  delete []npartners;
  delete []pairstyle;
}

/* ---------------------------------------------------------------------- */

void FixWallGran::pre_delete()
{
  //delete history fix
  if (wallstyle == MESHGRAN_FWG) modify->delete_fix("fix_neigh_tri");
}

/* ---------------------------------------------------------------------- */

int FixWallGran::setmask()
{
  int mask = 0;
  mask |= POST_FORCE;
  mask |= POST_FORCE_RESPA;
  return mask;
}

/* ---------------------------------------------------------------------- */

void FixWallGran::post_create()
{
    
    if ((wallstyle == MESHGRAN_FWG) && (fix_tri_neighlist == NULL)) registerTriNeighlist();

    //check if wall style and pair style fit together
    pairstyle = new char[strlen(style)-9+1];
    strcpy(pairstyle,&style[9]);

    PairGran *oldpair = pairgran;
    pairgran = (PairGran*)force->pair_match(pairstyle,0);
    bool pair_changed = (pairgran!=oldpair);
    if (!pairgran) error->all("Fix wall/gran style and pair/gran style have to match");

    //get history settings from pair style
    
    history = pairgran->history;
    if(dnum != 0 && dnum != pairgran->dnum) error->all("Number of contact history values of pair style and wall style does not match");
    else dnum = pairgran->dnum;
    
    grow_arrays(atom->nmax);

    //initialize as if particle is not touching wall
    if(pair_changed)
    {
        int nlocal = atom->nlocal;
        for (int i = 0; i < nlocal; i++)
        {
          npartners[i]=0;
          for (int k=0;k<maxpartners;k++)
          {
              partner[i][k][0] = partner[i][k][0] = -1;
              for(int d = 0; d < dnum; d++)
                  contacthistory[i][k][d] = 0.0;
          }
        }
    }
}

/* ---------------------------------------------------------------------- */

void FixWallGran::init()
{
    
    PairGran *oldpair = pairgran;
    
    pairgran = (PairGran*)force->pair_match(pairstyle,0);
    bool pair_changed = (pairgran!=oldpair);

    //initialize as if particle is not touching wall
    if(pair_changed)
    {
        history = pairgran->history;
        if(dnum != pairgran->dnum) error->all("Can not change to this pair style with wall/gran being active");
        grow_arrays(atom->nmax);
        int nlocal = atom->nlocal;
        for (int i = 0; i < nlocal; i++)
        {
          npartners[i]=0;
          for (int k=0;k<maxpartners;k++)
          {
              partner[i][k][0] = partner[i][k][0] = -1;
              for(int d = 0; d < dnum; d++)
                  contacthistory[i][k][d] = 0.0;
          }
        }
    }

    fr = pairgran->fr;

    dt = update->dt;

    if (strcmp(update->integrate_style,"respa") == 0)
      nlevels_respa = ((Respa *) update->integrate)->nlevels;

    init_substyle();
}

/* ---------------------------------------------------------------------- */

void FixWallGran::setup(int vflag)
{
  
  init_heattransfer();

  if ((wallstyle==MESHGRAN_FWG) && (fix_tri_neighlist==NULL)) registerTriNeighlist();

  if (strcmp(update->integrate_style,"verlet") == 0)
    post_force(vflag);
  else {
    ((Respa *) update->integrate)->copy_flevel_f(nlevels_respa-1);
    post_force_respa(vflag,nlevels_respa-1,0);
    ((Respa *) update->integrate)->copy_f_flevel(nlevels_respa-1);
  }
}

/* ---------------------------------------------------------------------- */

void FixWallGran::registerTriNeighlist()
{
    char **fixarg;
    fixarg=new char*[4];
    for (int kk=0;kk<4;kk++) fixarg[kk]=new char[40];
    //register  neighborlist fix
    fixarg[0]="fix_neigh_tri";
    fixarg[1]="all";
    fixarg[2]="neighlist/tri";
    strcpy(fixarg[3],id);
    modify->add_fix(4,fixarg);
    delete []fixarg;

    int i_fix=modify->find_fix("fix_neigh_tri");
    if (i_fix==-1) error->all("Could not register a fix for the triangle neighbor list");

    fix_tri_neighlist=static_cast<FixTriNeighlist*>(modify->fix[i_fix]);

}

/* ---------------------------------------------------------------------- */

void FixWallGran::init_heattransfer()
{
    fppa_T = NULL;
    fppa_hf = NULL;
    deltan_ratio = NULL;
    if (wallstyle != MESHGRAN_FWG && Temp_wall < 0.) return;
    else if (wallstyle == MESHGRAN_FWG)
    {
        int heatflag = 0;
        for(int imesh = 0; imesh < nFixMeshGran; imesh++)
            heatflag = heatflag || FixMeshGranList[imesh]->Temp_mesh >= 0.;
        if(!heatflag) return;
    }
    fppa_T = static_cast<FixPropertyPerAtom*>(modify->fix[modify->find_fix_property("Temp","property/peratom","scalar",1,0)]);
    fppa_hf = static_cast<FixPropertyPerAtom*>(modify->fix[modify->find_fix_property("heatFlux","property/peratom","scalar",1,0)]);

    th_cond = static_cast<FixPropertyGlobal*>(modify->fix[modify->find_fix_property("thermalConductivity","property/global","peratomtype",0,0)])->get_values();

    int ifix = modify->find_fix_property("youngsModulusOriginal","property/global","peratomtype",0,0,false);
    if(ifix >= 0) deltan_ratio = static_cast<FixPropertyGlobal*>(modify->fix[ifix])->get_array_modified();
}

/* ---------------------------------------------------------------------- */

void FixWallGran::post_force_respa(int vflag, int ilevel, int iloop)
{
  if (ilevel == nlevels_respa-1) post_force(vflag);
}

/* ----------------------------------------------------------------------
   add tri to contact list; if already there, just return the index
   if contact list not large enough, grow it
------------------------------------------------------------------------- */

inline int FixWallGran::add_to_contact_list(int i,int iFMG,int iTri)
{
    for(int k = 0; k < npartners[i]; k++)
        if(partner[i][k][0] == iFMG && partner[i][k][1] == iTri) return k;

    if(npartners[i] == maxpartners) grow_arrays_maxtritouch(atom->nmax);

    partner[i][npartners[i]][0] = iFMG;
    partner[i][npartners[i]][1] = iTri;
    npartners[i]++;

    if(history) history_transition(i,iFMG,iTri);

    return (npartners[i]-1);
}

/* ---------------------------------------------------------------------- */

inline void FixWallGran::history_transition(int i,int iFMG,int iTri)
{
    
    for(int j = 0; j < (npartners[i]-1); j++)
    {
        if (partner[i][j][0] != iFMG) continue;
        int iPartner = partner[i][j][1];
        if(FixMeshGranList[iFMG]->STLdata->are_coplanar_neighs(iTri,iPartner))
        {
            
                for(int d = 0; d < dnum; d++)
                   contacthistory[i][npartners[i]-1][d] = contacthistory[i][j][d];
                
        }
    }
}

/* ----------------------------------------------------------------------
   remove tri from contact list; also reset shear history
------------------------------------------------------------------------- */

inline void FixWallGran::remove_from_contact_list(int i,int iFMG,int iTri)
{
    for(int k=0;k<npartners[i];k++)
    {
        if(partner[i][k][0]==iFMG && partner[i][k][1]==iTri)
        {
            if(npartners[i] > 1) //swap with last contact if more than 1
            {
                partner[i][k][0] = partner[i][npartners[i]-1][0];
                partner[i][k][1] = partner[i][npartners[i]-1][1];
                if(history) copy_history(i,k);

            }
            partner[i][npartners[i]-1][0] = -1;
            partner[i][npartners[i]-1][1] = -1;
            if(history) reset_history(i,npartners[i]-1);
            npartners[i]--;
        }
    }
    return;
}

void FixWallGran::remove_from_contact_list_ext(int i,int iFMG,int iTri)
{
    remove_from_contact_list(i,iFMG,iTri);
}

/* ---------------------------------------------------------------------- */

inline void FixWallGran::reset_history(int i,int j)
{
    if (contacthistory) {
        for(int d = 0; d < dnum; d++)
           contacthistory[i][j][d] = 0.0;
    }
    else error->all("wall/gran: contact history error during reset");
}

/* ---------------------------------------------------------------------- */

inline void FixWallGran::copy_history(int i,int j)
{
    if (contacthistory) {
        for(int d = 0; d < dnum; d++)
           contacthistory[i][j][d] = contacthistory[i][npartners[i]-1][d];
    }
    else error->all("wall/gran: contact history error during copy");
}

/* ---------------------------------------------------------------------- */

void FixWallGran::post_force(int vflag)
{
  double vwall[3],dx,dy,dz,del1,del2,delxy,delr,rsq,area_ratio;

  // set position of wall to initial settings and velocity to 0.0
  // if wiggle or shear, set wall position and velocity accordingly
  
  double wlo = lo;
  double whi = hi;
  vwall[0] = vwall[1] = vwall[2] = 0.0;
  if (wiggle) {
    double arg = omega * (update->ntimestep - time_origin) * dt;
    wlo = lo + amplitude*sin(arg);//amplitude - amplitude*cos(arg);
    whi = hi + amplitude*sin(arg);//amplitude - amplitude*cos(arg);
    vwall[axis] = amplitude*omega*cos(arg);//amplitude*omega*sin(arg);
  } else if (wshear) vwall[axis] = vshear;

  // loop over all my atoms
  // rsq = distance from wall
  // dx,dy,dz = signed distance from wall
  // for rotating cylinder, reset vwall based on particle position
  // skip atom if not close enough to wall
  //   if wall was set to NULL, it's skipped since lo/hi are infinity
  // compute force and torque on atom if close enough to wall
  //   via wall potential matched to pair potential
  // set shear if pair potential stores history

  double **x = atom->x;
  double **v = atom->v;
  double **f = atom->f;
  double **omega = atom->omega;
  double **torque = atom->torque;
  double *radius = atom->radius;
  double *rmass = atom->rmass;
  int *mask = atom->mask;
  int nlocal = atom->nlocal;
  double *shr;

  if(fppa_T && fppa_hf)
  {
      Temp_p = fppa_T->vector_atom;
      heatflux = fppa_hf->vector_atom;
  }

  int iTri,iFMG;
  double deltan;
  double en0[3];
  bool excl;
  int *nTriList;
  int ***tri_neighlist;
  int *delflag;

  int iContactList;

  double contactPoint[3];
  double contactPoint_triCoo[3];
  double **vnode, *c_history;
  double Mt[3][3]={{0.,0.,0.},{0.,0.,0.},{0.,0.,0.}};
  double wallforce[3];
  double tmp[3],tmp2[3];

  if (update->ntimestep > laststep) shearupdate = 1;
  else shearupdate = 0;

  if(wallstyle == MESHGRAN_FWG) {
      if(fix_tri_neighlist==NULL) error->all("fatal: Could not find triangle neighbor list");
      nTriList=fix_tri_neighlist->nTriList;
      tri_neighlist=fix_tri_neighlist->tri_neighlist;
      delflag=fix_tri_neighlist->delflag;

      //zero wall forces
      reset_wall_forces();
      
  }
  
  for (int i = 0; i < nlocal; i++) {
    if (mask[i] & groupbit) {

      dx = dy = dz = 0.0;

      if (wallstyle == XPLANE_FWG) {
	    del1 = x[i][0] - wlo;
	    del2 = whi - x[i][0];
	    if (del1 < del2) dx = del1;
	    else dx = -del2;
      } else if (wallstyle == YPLANE_FWG) {
	    del1 = x[i][1] - wlo;
	    del2 = whi - x[i][1];
	    if (del1 < del2) dy = del1;
	    else dy = -del2;

      } else if (wallstyle == ZPLANE_FWG) {
	    del1 = x[i][2] - wlo;
	    del2 = whi - x[i][2];
	    if (del1 < del2) dz = del1;
	    else dz = -del2;
      } else if (wallstyle == ZCYLINDER_FWG) {
        delxy = sqrt(x[i][0]*x[i][0] + x[i][1]*x[i][1]);
        delr = cylradius - delxy;
	    if (delr > radius[i]) dz = cylradius;
	    else {
	      dx = -delr/delxy * x[i][0];
	      dy = -delr/delxy * x[i][1];
          if (wshear && axis != 2) {
	        vwall[0] = vshear * x[i][1]/delxy;
	        vwall[1] = -vshear * x[i][0]/delxy;
	        vwall[2] = 0.0;
	      }
	     }
      }

      //for style !=MESHGRAN, we have just one wall - handle it as we did in the old LAMMPS wall handling
      if (wallstyle != MESHGRAN_FWG){
          rsq = dx*dx + dy*dy + dz*dz;
          if (rsq > radius[i]*radius[i])
          {
              if(history) reset_history(i,0);
          }
          else
          {
             double deltan = radius[i]-sqrt(rsq);
             
             if(history) c_history = contacthistory[i][0];
             else c_history = NULL;

             double meff_wall = atom->rmass[i];
             if(fr&&fr->body[i]>=0)
             {
                meff_wall=fr->masstotal[fr->body[i]];  
             }

             compute_force(i, deltan,rsq,meff_wall, dx, dy, dz, vwall, c_history, 1);
             if(fppa_T && fppa_hf && Temp_wall > 0.) addHeatFlux( i, rsq, 1);
            
          }
      }
      else //wallstyle == MESHGRAN
      {
          
          for (int j=0;j<nTriList[i];j++)
          {
              
              delflag[j] = 0;

              iFMG = tri_neighlist[i][j][0];
              iTri = tri_neighlist[i][j][1];

              deltan = TRISPHERE::resolveTriSphereContact(lmp,i,iTri,FixMeshGranList[iFMG]->STLdata,F_SHRINKAGE,en0,false,0.,excl);

              if (deltan < 0.) //overlap
              {
                  //add to list of contacts
                  iContactList = add_to_contact_list(i,iFMG,iTri);

                  vectorScalarMult3D(en0,atom->radius[i],contactPoint);
                  vectorAdd3D(atom->x[i],contactPoint,contactPoint);

                  if(FixMeshGranList[iFMG]->STLdata->movingMesh)
                  {
                      
                      for (int m1=0;m1<3;m1++){
                          for (int m2=0;m2<3;m2++){
                              Mt[m1][m2]=FixMeshGranList[iFMG]->STLdata->node[iTri][m2][m1];
                          }
                      }
                      
                      if(fabs(MathExtra::mdet(Mt,lmp->error))>EPSILON_MOVINGMESH) MathExtra::mldivide3(Mt,contactPoint,contactPoint_triCoo,lmp->error);

                      else  
                      {
                          vectorCross3D(FixMeshGranList[iFMG]->STLdata->node[iTri][0],FixMeshGranList[iFMG]->STLdata->node[iTri][1],tmp);
                          if (vectorMag3D(tmp)>EPSILON_MOVINGMESH) for(int m1=0;m1<3;m1++) Mt[m1][2]=tmp[m1];
                          else {
                              vectorCross3D(FixMeshGranList[iFMG]->STLdata->node[iTri][0],FixMeshGranList[iFMG]->STLdata->node[iTri][2],tmp);
                              if (vectorMag3D(tmp)>EPSILON_MOVINGMESH) for(int m1=0;m1<3;m1++) Mt[m1][1]=tmp[m1];
                              else {
                                 vectorCross3D(FixMeshGranList[iFMG]->STLdata->node[iTri][1],FixMeshGranList[iFMG]->STLdata->node[iTri][2],tmp);
                                 if (vectorMag3D(tmp)>EPSILON_MOVINGMESH) for(int m1=0;m1<3;m1++) Mt[m1][0]=tmp[m1];
                              }
                          }
                          MathExtra::mldivide3(Mt,contactPoint,contactPoint_triCoo,lmp->error);
                      }

                          //to reduce numerical error, normalize the coordinates manually
                          normalize_bary(contactPoint_triCoo);

                          vnode=FixMeshGranList[iFMG]->STLdata->v_node[iTri];
                          for (int mm=0;mm<3;mm++) {
                             vwall[mm]=contactPoint_triCoo[0]*vnode[0][mm]+contactPoint_triCoo[1]*vnode[1][mm]+contactPoint_triCoo[2]*vnode[2][mm];
                          }
                      
                  }
                  else if(FixMeshGranList[iFMG]->STLdata->conveyor) for(int mm=0;mm<3;mm++) vwall[mm]=FixMeshGranList[iFMG]->STLdata->v_node[iTri][0][mm]; //all nodes have same vel
                  else for(int mm=0;mm<3;mm++) vwall[mm]=0.;

                  atom_type_wall=FixMeshGranList[iFMG]->atom_type_wall;
                  Temp_wall = FixMeshGranList[iFMG]->Temp_mesh;

                  if(history) c_history = contacthistory[i][iContactList];
                  else c_history = NULL;

                  delr = radius[i]+deltan; //deltan<0
                  rsq = delr*delr;
                  dx = -delr*en0[0];
                  dy = -delr*en0[1];
                  dz = -delr*en0[2];

                  double tri_area = FixMeshGranList[iFMG]->STLdata->Area[iTri];
                  double contact_area = (radius[i]*radius[i]-rsq)*M_PI; //contact area sphere-wall
                  if(tri_area > contact_area) area_ratio = 1.;
                  else area_ratio = tri_area / contact_area;

                  double meff_wall = atom->rmass[i];
                  if(fr&&fr->body[i]>=0)
                  {
                      meff_wall=fr->masstotal[fr->body[i]];  
                  }

                  if (FixMeshGranList[iFMG]->analyseStress)
                  {
                        //calculate force on particle and wall triangle
                        vectorCopy3D(f[i],wallforce);
                        compute_force(i,-deltan,rsq,meff_wall, dx, dy, dz, vwall, c_history,area_ratio);
                        vectorSubtract3D(wallforce,f[i],wallforce);
                        FixMeshGranList[iFMG]->add_particle_contribution(wallforce,contactPoint,iTri,i);
                  }
                  else compute_force(i,-deltan,rsq,meff_wall, dx, dy, dz, vwall, c_history,area_ratio);

                  if(fppa_T && fppa_hf && Temp_wall >= 0.) addHeatFlux(i,rsq,area_ratio);

              }
              
              else if(npartners[i]) delflag[j] = 1;
          }

          for(int j=0; j < nTriList[i]; j++)
          {
              //unset touching neighs
              if(delflag[j]) remove_from_contact_list(i,tri_neighlist[i][j][0]/*iFMG*/,tri_neighlist[i][j][1]/*iTri*/);
          }
      }
    }
  }
  
  int maxpartners_all;
  MPI_Allreduce(&maxpartners,&maxpartners_all,1,MPI_INT,MPI_MAX,world);
  
  while(maxpartners < maxpartners_all)
  {
      grow_arrays_maxtritouch(atom->nmax);
  }

  laststep = update->ntimestep;
}

/* ----------------------------------------------------------------------
   zero wall forces
------------------------------------------------------------------------- */

inline void FixWallGran::reset_wall_forces()
{
      for (int i=0;i<nFixMeshGran;i++)
          for(int j=0;j<FixMeshGranList[i]->nTri;j++)
              for(int k=0;k<3;k++)
                 FixMeshGranList[i]->STLdata->f_tri[j][k]=0.;
}

/* ----------------------------------------------------------------------
   memory usage of local atom-based arrays
------------------------------------------------------------------------- */

double FixWallGran::memory_usage()
{
  int nmax = atom->nmax;
  double bytes = nmax * sizeof(int);
  bytes += maxpartners * 2*nmax * sizeof(int);

  if(!history) return bytes;
  bytes += maxpartners * dnum * nmax * sizeof(double);

  return bytes;
}

/* ----------------------------------------------------------------------
   allocate local atom-based arrays
------------------------------------------------------------------------- */

void FixWallGran::grow_arrays(int nmax)
{
  npartners = (int*)(memory->srealloc(npartners, nmax*sizeof(int), "fix_wall_gran:npartners"));
  partner = memory->grow_3d_int_array(partner,nmax,maxpartners,2,"fix_wall_gran:partner");

  if(!history)return;
  contacthistory = memory->grow_3d_double_array(contacthistory,nmax,maxpartners,dnum,"fix_wall_gran:contacthistory");
  
}

/* ----------------------------------------------------------------------
   grow max. # of touching tris 
------------------------------------------------------------------------- */

void FixWallGran::grow_arrays_maxtritouch(int nmax)
{
  if(comm->me == 0) {
    if(screen) fprintf(screen,"INFO: Maxmimum number of particle-tri contacts >%d at step %d, growing array\n",maxpartners,update->ntimestep);
    if(logfile) fprintf(logfile,"INFO: Maxmimum number of particle-tri contacts >%d at step %d, growing array\n",maxpartners,update->ntimestep);
  }
  maxpartners += DELTA_TRI_CONTACTS;
  
  int ***partner_g = memory->create_3d_int_array(nmax,maxpartners,2,"fix_wall_gran:partner_g");

  for(int i = 0; i < nmax; i++)
    for(int j = 0; j < maxpartners-DELTA_TRI_CONTACTS; j++)
      for(int k = 0; k < 2; k++)
         partner_g[i][j][k]=partner[i][j][k];

  int ***h = partner;
  partner = partner_g;
  memory->destroy_3d_int_array(h);

  if(history)
  {
      double ***contacthistory_g = memory->create_3d_double_array(nmax,maxpartners,dnum,"fix_wall_gran:contacthistory_g");

      for(int i = 0; i < nmax; i++)
        for(int j = 0; j < maxpartners-DELTA_TRI_CONTACTS; j++)
          for(int k = 0; k < dnum; k++)
             contacthistory_g[i][j][k] = contacthistory[i][j][k];

      double ***h2 = contacthistory;
      contacthistory = contacthistory_g;
      memory->destroy_3d_double_array(h2);
  }
}

/* ----------------------------------------------------------------------
   copy values within local atom-based arrays
------------------------------------------------------------------------- */

void FixWallGran::copy_arrays(int i, int j)
{
  npartners[j]=npartners[i];
  for (int k=0;k<maxpartners;k++)
  {
     partner[j][k][0] = partner[i][k][0];
     partner[j][k][1] = partner[i][k][1];
  }

  if(!history)return;

  for (int k=0;k<maxpartners;k++){
    for(int d = 0; d< dnum; d++)
       contacthistory[j][k][d] = contacthistory[i][k][d];
  }
}

/* ----------------------------------------------------------------------
   initialize one atom's array values, called when atom is created
------------------------------------------------------------------------- */

void FixWallGran::set_arrays(int i)
{
  npartners[i]=0;
  for (int k = 0; k < maxpartners; k++)
  {
     partner[i][k][0] = -1;
     partner[i][k][1] = -1;
  }

  if(!history) return;
  for (int k = 0; k < maxpartners; k++)
    for(int d = 0; d < dnum; d++)
      contacthistory[i][k][d] = 0.0;
}

/* ----------------------------------------------------------------------
   pack values in local atom-based arrays for exchange with another proc
------------------------------------------------------------------------- */

int FixWallGran::pack_exchange(int i, double *buf)
{
  int m=0;

  buf[m++] = static_cast<int>(npartners[i]);
  for (int k = 0; k < maxpartners; k++)
  {
     buf[m++] = static_cast<int>(partner[i][k][0]);
     buf[m++] = static_cast<int>(partner[i][k][1]);
  }

  if(!history) return m;

  for (int k = 0; k < maxpartners;k++)
    for(int d = 0; d < dnum; d++)
       buf[m++] = contacthistory[i][k][d];

  return m;
}

/* ----------------------------------------------------------------------
   unpack values into local atom-based arrays after exchange
------------------------------------------------------------------------- */

int FixWallGran::unpack_exchange(int nlocal, double *buf)
{
  int m=0;

  npartners[nlocal]=static_cast<int>(buf[m++]);

  for (int k=0;k<maxpartners;k++)
  {
     partner[nlocal][k][0] = static_cast<int>(buf[m++]);
     partner[nlocal][k][1] = static_cast<int>(buf[m++]);
  }

  if(!history)return m;

  for (int k = 0;k < maxpartners; k++)
    for(int d = 0; d < dnum; d++)
      contacthistory[nlocal][k][d] = buf[m++];

  return m;
}

/* ----------------------------------------------------------------------*/

void FixWallGran::write_restart(FILE * fp)
{
  int n = 0;
  double list[2];
  list[n++] = static_cast<double>(maxpartners);
  list[n++] = static_cast<double>(dnum);

  if (comm->me == 0) {
    int size = n * sizeof(double);
    fwrite(&size,sizeof(int),1,fp);
    fwrite(list,sizeof(double),n,fp);
  }
}

/* ----------------------------------------------------------------------*/

void FixWallGran::restart(char *buf)
{
  int n = 0;
  double *list = (double *) buf;
  int maxpartners_restart = static_cast<int> (list[n++]);
  int dnum_restart = static_cast<int> (list[n++]);
  dnum = dnum_restart;

  if(maxpartners_restart < 1) error->all("Error re-starting fix wall/gran, corrupt restart file?");

  grow_arrays(atom->nmax);

  while(maxpartners < maxpartners_restart)
  {
      if(comm->me == 0) fprintf(screen,"INFO: Growing number of particle-tri contacts out of restart\n");
      grow_arrays_maxtritouch(atom->nmax);
  }

  if (meshwall && !fix_tri_neighlist) registerTriNeighlist();
  if (meshwall) fix_tri_neighlist->do_warn_dangerous=0;
}

/* ----------------------------------------------------------------------
   pack values in local atom-based arrays for restart file
------------------------------------------------------------------------- */

int FixWallGran::pack_restart(int i, double *buf)
{
  int m = 0;
  m++;
  buf[m++]=static_cast<double>(npartners[i]);
  for (int k = 0; k < maxpartners; k++)
  {
     buf[m++] = static_cast<double>(partner[i][k][0]);
     buf[m++] = static_cast<double>(partner[i][k][1]);
  }
  buf[0] = m;
  if(!history) return m;

  buf[0] += maxpartners*dnum;
  for (int k = 0; k < maxpartners; k++){
      for(int d = 0; d < dnum; d++)
         buf[m++] = contacthistory[i][k][d];
  }
  return m;
}

/* ----------------------------------------------------------------------
   unpack values from atom->extra array to restart the fix
------------------------------------------------------------------------- */

void FixWallGran::unpack_restart(int nlocal, int nth)
{
  double **extra = atom->extra;

  // skip to Nth set of extra values

  int m = 0;
  for (int i = 0; i < nth; i++) m += static_cast<int> (extra[nlocal][m]);
  m++;

  npartners[nlocal] = static_cast<int>(extra[nlocal][m++]);

  if(npartners[nlocal] > maxpartners)  error->all("Internal error: Failed to grow maxpartners, incompatible restart file.");

  for (int k = 0; k < maxpartners; k++){
    partner[nlocal][k][0] = static_cast<int>(extra[nlocal][m++]);
    partner[nlocal][k][1] = static_cast<int>(extra[nlocal][m++]);
  }

  if(!history) return;

  for (int k = 0; k < maxpartners; k++){
      for(int d = 0; d < dnum; d++)
        contacthistory[nlocal][k][d] = extra[nlocal][m++];
  }
}

/* ----------------------------------------------------------------------
   maxsize of any atom's restart data
------------------------------------------------------------------------- */

int FixWallGran::maxsize_restart()
{
  if(!history) return 1+1+maxpartners*2;
  return 1+1+maxpartners*(2+dnum);
}

/* ----------------------------------------------------------------------
   size of atom nlocal's restart data
------------------------------------------------------------------------- */

int FixWallGran::size_restart(int nlocal)
{
  if(!history) return 1+1+maxpartners*2;
  return 1+1+maxpartners*(2+dnum);
}

/* ---------------------------------------------------------------------- */

void FixWallGran::reset_dt()
{
  dt = update->dt;
}

/* ---------------------------------------------------------------------- */

int FixWallGran::n_contacts()
{
    if (wallstyle != MESHGRAN_FWG) return 0;

    int ncontacts = 0;
    int nlocal = atom->nlocal;

    for(int i = 0; i < nlocal; i++)
        ncontacts += npartners[i];

    return ncontacts;
}
