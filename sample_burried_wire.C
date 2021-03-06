#include <cmath>
#if !defined(__APPLE__)
#include "malloc.h"
#endif

#include "sample_burried_wire.h"

using namespace MyTRIM_NS;

SampleBurriedWire::SampleBurriedWire(Real x, Real y, Real z) :
    SampleWire(x, y, z)
{
 bc[0] = INF;
 bc[1] = INF;
 bc[2] = INF;
}

// look if we are within dr of the wire axis
MaterialBase*
SampleBurriedWire::lookupMaterial(Point & pos)
{
  // cover layer
  if (pos(2) < 0.0 && pos(2) >= -250.0)
    return material[1];

  // above sample or inside substrate
  if (pos(2) > w[2] || pos(2) < -250.0 )
    return 0;

  // in wire layer
  MaterialBase *ret = SampleWire::lookupMaterial(pos);
  if (ret == 0)
    return material[1];
  else
    return ret;
}
