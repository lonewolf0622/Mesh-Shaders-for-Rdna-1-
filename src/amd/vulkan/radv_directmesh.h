/* SPDX-License-Identifier: MIT */
#ifndef RADV_DIRECTMESH_H
#define RADV_DIRECTMESH_H

#include "amd_family.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static inline bool
radv_directmesh_family(enum radeon_family family)
{
   /* Navi14 is left out: upstream keeps NGG off there because of hardware
    * bugs, and the Mesh-only NGG path has not been run on that chip. */
   return family == CHIP_NAVI10 || family == CHIP_NAVI12 || family == CHIP_GFX1013;
}

/* Exact display-device IDs from Linux's amdgpu PCI table. Do not match a whole
 * PCI range: adjacent IDs include unrelated devices and audio functions. */
static inline bool
radv_directmesh_pci(uint16_t vendor, uint16_t device)
{
   if (vendor != 0x1002)
      return false;
   switch (device) {
   case 0x13fe:
   case 0x143f:
   case 0x7310: case 0x7312: case 0x7318: case 0x7319:
   case 0x731a: case 0x731b: case 0x731e: case 0x731f:
   case 0x7360: case 0x7362:
      return true;
   default:
      return false;
   }
}

static inline bool
radv_directmesh_preset(const char *option, bool detected)
{
   return option ? !strcmp(option, "1") : detected;
}

/* RADV_DIRECTMESH=1 set by the user (not only the automatic default). */
static inline bool
radv_directmesh_explicit(const char *option)
{
   return option && !strcmp(option, "1");
}

#ifdef __cplusplus
extern "C" {
#endif

struct radv_directmesh_setting {
   const char *name;
   const char *value;
};

const struct radv_directmesh_setting *radv_directmesh_settings_table(unsigned *count);
/* The preset value for name, or NULL when the preset does not set it. */
const char *radv_directmesh_default(const char *name);
/* env_value if set, else the preset value when the preset is active. */
const char *radv_directmesh_resolve(const char *env_value, const char *name, bool active);
void radv_directmesh_set_active(bool active);
bool radv_directmesh_is_active(void);

/* Option reads for DirectMesh switches: the environment wins, then the preset. */
const char *radv_dm_option(const char *name);
bool radv_dm_bool(const char *name, bool dfault);
int64_t radv_dm_num(const char *name, int64_t dfault);

#ifdef __cplusplus
}
#endif

#endif
