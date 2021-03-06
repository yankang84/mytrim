/***************************************************************************
 *   Copyright (C) 2008 by Daniel Schwen   *
 *   daniel@schwen.de   *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <queue>
#include <iostream>
#include <fstream>
#include <sstream>

#include "simconf.h"
#include "element.h"
#include "material.h"
#include "sample_wire.h"
#include "sample_burried_wire.h"
#include "ion.h"
#include "trim.h"
#include "invert.h"

#include "functions.h"

using namespace MyTRIM_NS;

int main(int argc, char *argv[])
{
  if (argc != 8)
  {
    std::cerr << "syntax: " << argv[0] << " basename angle[deg] diameter(nm) burried[0, 1] numbermultiplier xyzout[0, 1] lbinout[0, 1]" << std::endl;
    return 1;
  }

  Real theta = atof(argv[2]) * M_PI/180.0; // 0 = parallel to wire
  Real diameter  = 10.0*atof(argv[3]);
  Real length  = 11000.0; // 1.1 mu
  bool burried = (atoi(argv[4]) != 0);
  Real mult = atof(argv[5]);
  bool xyz_out  = (atoi(argv[6]) != 0);
  bool ldat_out = (atoi(argv[7]) != 0);

  // ion series
  const int nstep = 5;
  Real ion_dose[nstep] = { 3.0e13, 2.2e13, 1.5e13, 1.2e13, 2.5e13 }; // in ions/cm^2
  int ion_count[nstep];
  IonBase* ion_prototype[nstep];
  ion_prototype[0] = new IonBase( 5, 11.0 , 320.0e3); // Z, m, E
  ion_prototype[1] = new IonBase( 5, 11.0 , 220.0e3); // Z, m, E
  ion_prototype[2] = new IonBase( 5, 11.0 , 160.0e3); // Z, m, E
  ion_prototype[3] = new IonBase( 5, 11.0 , 120.0e3); // Z, m, E
  ion_prototype[4] = new IonBase(15, 31.0 , 250.0e3); // Z, m, E

  // seed random number generator from system entropy pool
  FILE *urand = fopen("/dev/random", "r");
  unsigned int seed;
  if (fread(&seed, sizeof(unsigned int), 1, urand) != 1) return 1;
  fclose(urand);

  // initialize global parameter structure and read data tables from file
  SimconfType * simconf = new SimconfType(seed);

  // initialize sample structure
  SampleWire *sample;
  if (burried)
    sample = new SampleBurriedWire(diameter, diameter, length);
  else
  {
    sample = new SampleWire(diameter, diameter, length);
    sample->bc[2] = SampleWire::CUT;
  }

  // calculate actual ion numbers
  for (int s = 0; s < nstep; ++s)
  {
    Real A; // irradiated area in Ang^2
    if (burried)
      A =(length + sample->w[0]) * (length + sample->w[1]);
    else
      A = cos(theta) * M_PI * 0.25 * sample->w[0] * sample->w[1] + //   slanted top face
          sin(theta) * length * sample->w[0];                      // + projected side

    // 1cm^2 = 1e16 Ang**2, 1Ang^2 = 1e-16cm^2
    ion_count[s] = ion_dose[s] * A * 1.0e-16 * mult;
    std::cerr << "Ion " << s << ' ' << ion_count[s] << std::endl;
  }

  // initialize trim engine for the sample
  /*  const int z1 = 31;
      const int z2 = 33;
      TrimVacMap *trim = new TrimVacMap(sample, z1, z2); // GaAs
  */
  //TrimBase *trim = new TrimBase(sample);
  TrimBase *trim = new TrimPrimaries(simconf, sample);

  MaterialBase *material;
  Element element;

  // Si
  material = new MaterialBase(simconf, 2.329); // rho
  element._Z = 14; // Si
  element._m = 28.0;
  element._t = 1.0;
  material->_element.push_back(element);
  material->prepare(); // all materials added
  sample->material.push_back(material); // add material to sample

  // SiO2 (material[1] for the cover layer in SampleBurriedWire)
  material = new MaterialBase(simconf, 2.634); // rho
  element._Z = 14; // Si
  element._m = 28.0;
  element._t = 1.0;
  material->_element.push_back(element);
  element._Z = 8; // O
  element._m = 16.0;
  element._t = 2.0;
  material->_element.push_back(element);
  material->prepare(); // all materials added
  sample->material.push_back(material); // add material to sample

  // create a FIFO for recoils
  std::queue<IonBase*> recoils;

  IonBase *pka;

  // map concentration along length
  int *lbins[2];
  int lx = 100; // 100 bins
  int dl = length/Real(lx);
  lbins[1] = new int[lx]; // P z=15
  for (int i = 0; i < 2; ++i)
  {
    lbins[i] = new int[lx]; // 0=B (z=5), 1=P (z=15)
    for (int l = 0; l < lx; ++l)
      lbins[i][l] = 0;
  }

  // xyz data
  int xyz_lines = 0;
  std::stringstream xyz_data;

  for (int s = 0; s < nstep; ++s)
  {
    for (int n = 0; n < ion_count[s]; ++n)
    {
      if (n % 10000 == 0)
        std::cerr << "pka #" << n+1 << std::endl;

      // generate new PKA from prototype ion
      pka = new IonBase(ion_prototype[s]);
      pka->_gen = 0; // generation (0 = PKA)
      pka->_tag = -1;

      pka->_dir(0) = 0.0;
      pka->_dir(1) = sin(theta);
      pka->_dir(2) = cos(theta);

      v_norm(pka->_dir);

      if (burried)
      {
        // cannot anticipate the straggling in the burrial layer, thus have to shoot onto a big surface
        // TODO: take theta into account!
        pka->_pos(0) = (simconf->drand() - 0.5) * (length + sample->w[0]);
        pka->_pos(1) = (simconf->drand() - 0.5) * (length + sample->w[1]);
        pka->_pos(2) = -250.0; // overcoat thickness
      }
      else
      {
        if (theta == 0.0)
        {
          // 0 degrees => start on top of wire!
          pka->_pos(2) = 0.0;
          do
          {
            pka->_pos(0) = simconf->drand() * sample->w[0];
            pka->_pos(1) = simconf->drand() * sample->w[1];
          } while (sample->lookupMaterial(pka->_pos) == 0);
        }
        else
        {
          // start on side _or_ top!
          Real vpos[3], t;
          do
          {
            do
            {
              vpos[0] = simconf->drand() * sample->w[0];
              vpos[1] = 0.0;
              vpos[2] = (simconf->drand() * (length + diameter/tan(theta))) - diameter/tan(theta);

              t = (1.0 - std::sqrt(1.0 - sqr(2*vpos[0]/diameter - 1.0))) * diameter/(2.0*pka->_dir(1));

              // if we start beyond wire length (that would be inside the substrate) then retry
            } while (t*pka->_dir(2) + vpos[2] >= length);

            // if first intersection with cylinder is at z<0 then check if we hit the top face instead
            if (t*pka->_dir(2) + vpos[2] < 0.0)
              t = -vpos[2]/pka->_dir(2);

            // start PKA at calculated intersection point
            for (int i = 0; i < 3; ++i)
                pka->_pos(i) = t*pka->_dir(i) + vpos[i];

          } while (sample->lookupMaterial(pka->_pos) == 0);
        }
      }
      //cout << "START " << pka->_pos(0) << ' ' << pka->_pos(1) << ' ' << pka->_pos(2) << ' ' << std::endl;
      //continue;

      pka->setEf();
      recoils.push(pka);

      while (!recoils.empty())
      {
        pka = recoils.front();
        recoils.pop();
        sample->averages(pka);

        // do ion analysis/processing BEFORE the cascade here

        if (pka->_Z == ion_prototype[s]->_Z )
        {
          //printf( "p1 %f\t%f\t%f\n", pka->_pos(0), pka->_pos(1), pka->_pos(2));
        }

        // follow this ion's trajectory and store recoils
        trim->trim(pka, recoils);

        // do ion analysis/processing AFTER the cascade here

        // ion is in the wire
        if ( sample->lookupMaterial(pka->_pos) == sample->material[0])
        {
          int l = pka->_pos(2) / dl;
          if (l >=0 && l < lx)
          {
            if (xyz_out)
            {
              xyz_data << simconf->scoef[pka->_Z-1].sym << ' '
                      << pka->_pos(0)/100.0 << ' ' << pka->_pos(1)/100.0 << ' ' << pka->_pos(2)/100.0 << std::endl;
              xyz_lines++;
            }

            if (ldat_out)
              lbins[ (pka->_Z == 5) ? 0 : 1 ][l]++;
          }
        }

        // done with this recoil
        delete pka;
      }
    }
  }

  // write xyz file
  if (xyz_out)
  {
    std::stringstream xyz_name;
    xyz_name << argv[1] << ".xyz";
    std::ofstream xyz(xyz_name.str().c_str());
    xyz << xyz_lines << std::endl << std::endl << xyz_data.str();
    xyz.close();
  }

  // write lbins file (atoms per nm^3)
  if (ldat_out)
  {
    std::stringstream ldat_name;
    ldat_name << argv[1] << ".ldat";
    std::ofstream ldat(ldat_name.str().c_str());
    Real dv = 1e-3 * dl * M_PI * 0.25 *sample->w[0] * sample->w[1]; // volume per bin in nm^3
    for (int l = 0; l < lx; ++l)
      ldat << l*dl << ' ' << lbins[0][l]/(mult*dv) << ' ' << lbins[1][l]/(mult*dv) << std::endl;
    ldat.close();
  }
  delete[] lbins[0];
  delete[] lbins[1];

  return EXIT_SUCCESS;
}
