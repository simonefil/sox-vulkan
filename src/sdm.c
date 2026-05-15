/* Sigma-Delta modulator
 * Copyright (c) 2015 Mans Rullgard <mans@mansr.com>
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

/*
 * References:
 *
 * Derk Reefman, Erwin Janssen. 2002.
 * "Signal processing for Direct Stream Digital: A tutorial for
 * digital Sigma Delta modulation and 1-bit digital audio processing"
 * http://www.emmlabs.com/pdf/papers/DerkSigmaDelta.pdf
 *
 * P.J.A. Harpe. 2003.
 * "Trellis-type Sigma Delta Modulators for Super Audio CD applications"
 * http://www.pieterharpe.nl/docs/report_trunc.pdf
 *
 * Richard Schreier. 2000-2011.
 * "Delta Sigma Toolbox"
 * http://www.mathworks.com/matlabcentral/fileexchange/19-delta-sigma-toolbox
 */

/*
 Damien Plisson <damien78@audirvana.com> Apr 20th, 2016
 Added 64bit sample => 8bit packets SDM processing functions
 */

#define _ISOC11_SOURCE

#include "sox_i.h"
#include "sdm.h"
#if HAVE_VULKAN
#include "rate_vulkan.h"
#include "sdm_vulkan.h"
#include "vulkan_effect_chain.h"
#endif
#include <assert.h>

#define MAX_FILTER_ORDER 8
#define SDM_STATE_LIMIT 1e100
#define PATH_HASH_SIZE 128
#define PATH_HASH_MASK (PATH_HASH_SIZE - 1)

typedef struct LSX_ALIGN(32) sdm_filter {
  const double  a[MAX_FILTER_ORDER];
  const double  g[MAX_FILTER_ORDER];
  int32_t       order;
  unsigned      freq;
  const char   *name;
  int           trellis_order;
  int           trellis_num;
  int           trellis_lat;
} sdm_filter_t;

typedef struct LSX_ALIGN(32) sdm_state {
  double        state[MAX_FILTER_ORDER];
  double        cost;
  uint32_t      path;
  uint8_t       next;
  uint8_t       hist;
  uint8_t       hist_used;
  struct sdm_state *parent;
  struct sdm_state *path_list;
} sdm_state_t;

typedef struct {
  sdm_state_t   sdm[2 * SDM_TRELLIS_MAX_NUM];
  sdm_state_t  *act[SDM_TRELLIS_MAX_NUM];
} sdm_trellis_t;

struct sdm {
  sdm_trellis_t trellis[2];
  sdm_state_t  *path_hash[PATH_HASH_SIZE];
  uint8_t       hist_free[2 * SDM_TRELLIS_MAX_NUM];
  unsigned      hist_fnum;
  uint32_t      trellis_mask;
  uint32_t      trellis_num;
  uint32_t      trellis_lat;
  unsigned      num_cands;
  unsigned      pos;
  unsigned      pending;
  unsigned      draining;
  unsigned      idx;
  unsigned      threads;
  unsigned      failed;
  const sdm_filter_t *filter;
  double        simple_state[MAX_FILTER_ORDER];
  double        prev_y;
  uint8_t       packet;
  uint8_t       packet_bits;
  uint64_t      conv_fail;
  uint8_t       hist[2 * SDM_TRELLIS_MAX_NUM][SDM_TRELLIS_MAX_LAT / 8];
};

//static sdm_filter_t sdm_filter_safe_64 = {
//  {
//    7.82475202270363e-01,
//    2.94699121204821e-01,
//    6.53377434565800e-02,
//    8.44052714292759e-03,
//    4.35748599576923e-04,
//  },
//  {
//    0, 6.98490600683106e-04,
//    0, 1.97734357803445e-03,
//  },
//  5,
//  64 * 44100,
//  "safe",
//  0, 0, 0,
//};
//
//static sdm_filter_t sdm_filter_safe_128 = {
//  {
//    7.81482424617463e-01,
//    2.95962741536345e-01,
//    6.66602230172735e-02,
//    8.83476316538580e-03,
//    5.29668589657667e-04,
//  },
//  {
//    0, 1.74653153894942e-04,
//    0, 4.94580504383930e-04,
//  },
//  5,
//  128 * 44100,
//  "safe",
//  0, 0, 0,
//};
//
//static sdm_filter_t sdm_filter_safe_256 = {
//  {
//    7.81234241629436e-01,
//    2.96279053797020e-01,
//    6.69905169222529e-02,
//    8.93375697799549e-03,
//    5.53589139903277e-04,
//  },
//  {
//    0, 4.36651951230006e-05,
//    0, 1.23660417994961e-04,
//  },
//  5,
//  256 * 44100,
//  "safe",
//  0, 0, 0,
//};
//
//static sdm_filter_t sdm_filter_fast_64 = {
//  {
//    8.73732185715521e-01,
//    3.66826120770180e-01,
//    9.05162830890437e-02,
//    1.29755185367144e-02,
//    7.61342556713979e-04,
//  },
//  {
//    0, 6.98490600683106e-04,
//    0, 1.97734357803445e-03,
//  },
//  5,
//  64 * 44100,
//  "fast",
//  0, 0, 0,
//};
//
//static sdm_filter_t sdm_filter_fast_128 = {
//  {
//    8.72742229753860e-01,
//    3.68009768304588e-01,
//    9.19659940843951e-02,
//    1.34616480046685e-02,
//    8.91081609405001e-04,
//  },
//  {
//    0, 1.74653153894942e-04,
//    0, 4.94580504383930e-04,
//  },
//  5,
//  128 * 44100,
//  "fast",
//  0, 0, 0,
//};
//
//static sdm_filter_t sdm_filter_fast_256 = {
//  {
//    8.72494751607231e-01,
//    3.68306085460265e-01,
//    9.23281294511712e-02,
//    1.35835992766018e-02,
//    9.24000248506501e-04,
//  },
//  {
//    0, 4.36651951230006e-05,
//    0, 1.23660417994961e-04,
//  },
//  5,
//  256 * 44100,
//  "fast",
//  0, 0, 0,
//};


static const sdm_filter_t sdm_filters[] = {
#include "sdm_high_rate_coefficients.inc"
  {
    {
      7.85192429016760e-01,
      2.98899796337831e-01,
      7.05344069837308e-02,
      1.10501947024035e-02,
      1.07505576489901e-03,
      6.64106274616966e-05,
      -5.74580397150193e-09,
    },
    {
      0, 5.18282192016751e-04,
      0, 1.72954362492419e-03,
      0, 2.83233339830110e-03,
    },
    7,
    64 * 44100,
    "hq",
    0, 0, 0,
  },
  
  {
    {
      7.83148010097334e-01,
      3.01437231238902e-01,
      7.31891646224574e-02,
      1.20314098366875e-02,
      1.32077937193861e-03,
      9.11181979169687e-05,
      2.59895240306562e-06,
    },
    {
      0, 9.92163123766340e-05,
      0, 3.31199917300393e-04,
      0, 5.42540771343282e-04,
    },
    7,
    128 * 44100,
    "hq",
    0, 0, 0,
  },
  
  {
    {
      7.82785077952658e-01,
      3.01888671316811e-01,
      7.36594376027782e-02,
      1.22068270909817e-02,
      1.36572694403914e-03,
      9.57806082134936e-05,
      3.13239368043838e-06,
    },
    {
      0, 2.48046933669715e-05,
      0, 8.28068362972358e-05,
      0, 1.35653594733585e-04,
    },
    7,
    256 * 44100,
    "hq",
    0, 0, 0,
  },
  
  {
    {
      1.00323940832478e+00,
      3.54975562370606e-01,
      5.64754047673194e-02,
      3.99067228430322e-03,
    },
    {
      1.74071110561285e-05, 0,
      1.11672812199443e-04, 0,
    },
    4,
    256 * 44100,
    "clans-4",
    0, 0, 0
  },
  {
    {
      8.69746397840960e-01,
      3.58080546314756e-01,
      8.02654082306273e-02,
      8.06528716282692e-03,
    },
    {
      1.74071110561285e-05, 0,
      1.11672812199443e-04, 0,
    },
    4,
    256 * 44100,
    "sdm-4",
    0, 0, 0
  },
  {
    {
      1.10212073518628e+00,
      4.33447134954244e-01,
      7.17865111532609e-02,
      4.48367825425951e-03,
      8.60861641068938e-05,
    },
    {
      0, 4.36651951230006e-05,
      0, 1.23660417994961e-04,
    },
    5,
    256 * 44100,
    "clans-5",
    0, 0, 0
  },
  {
    {
      8.07768375734983e-01,
      3.16440095967511e-01,
      7.38231738259889e-02,
      1.01432044963374e-02,
      6.46658652275506e-04,
    },
    {
      0, 4.36651951230006e-05,
      0, 1.23660417994961e-04,
    },
    5,
    256 * 44100,
    "sdm-5",
    0, 0, 0
  },
  {
    {
      9.97000121097967e-01,
      3.46002867430604e-01,
      5.74352078895161e-02,
      4.96197900435677e-03,
      2.16319301330580e-04,
      3.45938007947910e-06,
    },
    {
      8.57500543083848e-06, 0,
      6.58398680532347e-05, 0,
      1.30939362595793e-04, 0,
    },
    6,
    256 * 44100,
    "clans-6",
    0, 0, 0
  },
  {
    {
      8.08851952379691e-01,
      3.20414766828429e-01,
      7.85858596284593e-02,
      1.24781319607895e-02,
      1.21202847406105e-03,
      5.51622876557856e-05,
    },
    {
      8.57500543083848e-06, 0,
      6.58398680532347e-05, 0,
      1.30939362595793e-04, 0,
    },
    6,
    256 * 44100,
    "sdm-6",
    0, 0, 0
  },
  {
    {
      1.10629931445134e+00,
      4.22135693734657e-01,
      7.54595882135669e-02,
      7.07164815703843e-03,
      3.53092575577382e-04,
      8.89662856104825e-06,
      5.79674109824069e-08,
    },
    {
      0, 2.48046933669715e-05,
      0, 8.28068362972358e-05,
      0, 1.35653594733585e-04,
    },
    7,
    256 * 44100,
    "clans-7",
    0, 0, 0
  },
  {
    {
      7.82785077952658e-01,
      3.01888671316811e-01,
      7.36594376027782e-02,
      1.22068270909817e-02,
      1.36572694403914e-03,
      9.57806082134936e-05,
      3.13239368043838e-06,
    },
    {
      0, 2.48046933669715e-05,
      0, 8.28068362972358e-05,
      0, 1.35653594733585e-04,
    },
    7,
    256 * 44100,
    "sdm-7",
    0, 0, 0
  },
  {
    {
      1.15188624720851e+00,
      5.45054196257555e-01,
      1.38703640845632e-01,
      2.07076444822072e-02,
      1.85506614417771e-03,
      9.63403135615390e-05,
      2.69174565706992e-06,
      2.22594461751768e-08,
    },
    {
      5.06749566262594e-06, 0,
      4.15924517416912e-05, 0,
      9.55783346944871e-05, 0,
      1.38868728742641e-04, 0,
    },
    8,
    256 * 44100,
    "clans-8",
    0, 0, 0
  },
  {
    {
      7.42329617949054e-01,
      2.72509195471757e-01,
      6.41424039739473e-02,
      1.05299412132258e-02,
      1.23178223428228e-03,
      9.94985029720342e-05,
      5.13169547054423e-06,
      1.20466411041020e-07,
    },
    {
      5.06749566262594e-06, 0,
      4.15924517416912e-05, 0,
      9.55783346944871e-05, 0,
      1.38868728742641e-04, 0,
    },
    8,
    256 * 44100,
    "sdm-8",
    0, 0, 0
  },
  {
    {
      1.19985242167687e+00,
      5.39366678861047e-01,
      1.07433710905069e-01,
      7.85649993434925e-03,
    },
    {
      6.96272321944526e-05, 0,
      4.46641365529834e-04, 0,
    },
    4,
    128 * 44100,
    "clans-4",
    0, 0, 0
  },
  {
    {
      8.69935494013007e-01,
      3.57844753369190e-01,
      8.00232187246903e-02,
      7.95176796646842e-03,
    },
    {
      6.96272321944526e-05, 0,
      4.46641365529834e-04, 0,
    },
    4,
    128 * 44100,
    "sdm-4",
    0, 0, 0
  },
  {
    {
      1.12849522129362e+00,
      5.02128177800632e-01,
      1.10084368682902e-01,
      1.18635667860902e-02,
      4.71059243536326e-04,
    },
    {
      0, 1.74653153894942e-04,
      0, 4.94580504383930e-04,
    },
    5,
    128 * 44100,
    "clans-5",
    0, 0, 0
  },
  {
    {
      8.08016362125685e-01,
      3.16129639744972e-01,
      7.34835047943110e-02,
      1.00377576971692e-02,
      6.20309683440734e-04,
    },
    {
      0, 1.74653153894942e-04,
      0, 4.94580504383930e-04,
    },
    5,
    128 * 44100,
    "sdm-5",
    0, 0, 0
  },
  {
    {
      1.13839804508630e+00,
      5.16338264778321e-01,
      1.20760874713903e-01,
      1.53496744585395e-02,
      1.00733946588732e-03,
      2.18223963130981e-05,
    },
    {
      3.42997276004814e-05, 0,
      2.63342132660038e-04, 0,
      5.23688869916463e-04, 0,
    },
    6,
    128 * 44100,
    "clans-6",
    0, 0, 0
  },
  {
    {
      8.09157514480151e-01,
      3.20038611545599e-01,
      7.81955723892726e-02,
      1.23074728674017e-02,
      1.18346416730106e-03,
      5.04301224894810e-05,
    },
    {
      3.42997276004814e-05, 0,
      2.63342132660038e-04, 0,
      5.23688869916463e-04, 0,
    },
    6,
    128 * 44100,
    "sdm-6",
    0, 0, 0
  },
  {
    {
      8.98180853333862e-01,
      3.27985497323439e-01,
      6.38803466871112e-02,
      7.18262647412857e-03,
      4.51845004995476e-04,
      1.49685651672331e-05,
      4.22554681245302e-08,
    },
    {
      0, 9.92163123766340e-05,
      0, 3.31199917300393e-04,
      0, 5.42540771343282e-04,
    },
    7,
    128 * 44100,
    "clans-7",
    0, 0, 0
  },
  {
    {
      7.83148010097334e-01,
      3.01437231238902e-01,
      7.31891646224574e-02,
      1.20314098366875e-02,
      1.32077937193861e-03,
      9.11181979169687e-05,
      2.59895240306562e-06,
    },
    {
      0, 9.92163123766340e-05,
      0, 3.31199917300393e-04,
      0, 5.42540771343282e-04,
    },
    7,
    128 * 44100,
    "sdm-7",
    0, 0, 0
  },
  {
    {
      1.04472698053970e+00,
      4.62088167600438e-01,
      1.13484722685479e-01,
      1.68939738398161e-02,
      1.55891676875336e-03,
      8.23864822188133e-05,
      2.39690238375972e-06,
      -1.75063180618551e-09,
    },
    {
      2.02698799324546e-05, 0,
      1.66362887238597e-04, 0,
      3.82276797905696e-04, 0,
      5.55397776875272e-04, 0,
    },
    8,
    128 * 44100,
    "clans-8",
    0, 0, 0
  },
  {
    {
      7.42763211426562e-01,
      2.71983157679393e-01,
      6.36389361390464e-02,
      1.03289230528372e-02,
      1.19045645863092e-03,
      9.25357160397986e-05,
      4.64982367004083e-06,
      8.14280266547840e-08,
    },
    {
      2.02698799324546e-05, 0,
      1.66362887238597e-04, 0,
      3.82276797905696e-04, 0,
      5.55397776875272e-04, 0,
    },
    8,
    128 * 44100,
    "sdm-8",
    0, 0, 0
  },
  {
    {
      1.27879853057675e+00,
      6.11303913722028e-01,
      1.28497083869344e-01,
      9.36669621421730e-03,
    },
    {
      2.78489536971958e-04, 0,
      1.78576750808173e-03, 0,
    },
    4,
    64 * 44100,
    "clans-4",
    0, 0, 0
  },
  {
    {
      8.70691905361989e-01,
      3.56902669565715e-01,
      7.90540396115068e-02,
      7.49922172520510e-03,
    },
    {
      2.78489536971958e-04, 0,
      1.78576750808173e-03, 0,
    },
    4,
    64 * 44100,
    "sdm-4",
    0, 0, 0
  },
  {
    {
      1.09979653514762e+00,
      4.81149952106030e-01,
      1.03481231987752e-01,
      1.07520561970131e-02,
      3.08801118488355e-04,
    },
    {
      0, 6.98490600683106e-04,
      0, 1.97734357803445e-03,
    },
    5,
    64 * 44100,
    "clans-5",
    0, 0, 0
  },
  {
    {
      8.09008352716413e-01,
      3.14889441429587e-01,
      7.21235639855639e-02,
      9.61769014140330e-03,
      5.16726747047100e-04,
    },
    {
      0, 6.98490600683106e-04,
      0, 1.97734357803445e-03,
    },
    5,
    64 * 44100,
    "sdm-5",
    0, 0, 0
  },
  {
    {
      1.07903996429881e+00,
      4.81889508657128e-01,
      1.12960470418260e-01,
      1.41786764681378e-02,
      8.90696638761455e-04,
      3.12209321540191e-06,
    },
    {
      1.37194204516672e-04, 0,
      1.05309113432481e-03, 0,
      2.09365847953595e-03, 0,
    },
    6,
    64 * 44100,
    "clans-6",
    0, 0, 0
  },
  {
    {
      8.10379824203071e-01,
      3.18536209193388e-01,
      7.66325098232035e-02,
      1.16280270347611e-02,
      1.07113013239551e-03,
      3.23564051386283e-05,
    },
    {
      1.37194204516672e-04, 0,
      1.05309113432481e-03, 0,
      2.09365847953595e-03, 0,
    },
    6,
    64 * 44100,
    "sdm-6",
    0, 0, 0
  },
  {
    {
      1.30828743581024e+00,
      6.14252690035661e-01,
      1.30284958810903e-01,
      1.31280998331490e-02,
      4.80497172614556e-04,
      1.28747977598542e-07,
      -1.01500259908072e-06,
    },
    {
      0, 3.96825873999969e-04,
      0, 1.32436089566069e-03,
      0, 2.16898568341885e-03,
    },
    7,
    64 * 44100,
    "clans-7",
    0, 0, 0
  },
  {
    {
      7.84599817960974e-01,
      2.99634346028983e-01,
      7.13049276218066e-02,
      1.13334107086916e-02,
      1.14497642818158e-03,
      7.33018502803093e-05,
      6.80633400002018e-07,
    },
    {
      0, 3.96825873999969e-04,
      0, 1.32436089566069e-03,
      0, 2.16898568341885e-03,
    },
    7,
    64 * 44100,
    "sdm-7",
    0, 0, 0
  },
  {
    {
      1.18730059129261e+00,
      5.66733317291325e-01,
      1.40117339676942e-01,
      1.87599862200771e-02,
      1.27685506908071e-03,
      8.76397405988154e-06,
      -1.90294986721073e-06,
      -7.39020160622772e-08,
    },
    {
      8.10778762576884e-05, 0,
      6.65340842513387e-04, 0,
      1.52852264942192e-03, 0,
      2.22035724073886e-03, 0,
    },
    8,
    64 * 44100,
    "clans-8",
    0, 0, 0
  },
  {
    {
      7.44453769826547e-01,
      2.69850507860307e-01,
      6.16093616071757e-02,
      9.52771711245796e-03,
      1.02903114196526e-03,
      6.63758229311911e-05,
      2.91124056073927e-06,
      -4.29323230577427e-08,
    },
    {
      8.10778762576884e-05, 0,
      6.65340842513387e-04, 0,
      1.52852264942192e-03, 0,
      2.22035724073886e-03, 0,
    },
    8,
    64 * 44100,
    "sdm-8",
    0, 0, 0
  },
};

static const sdm_filter_t *sdm_find_filter(const char *name, unsigned freq)
{
  unsigned i;
  for (i = 0; i < array_length(sdm_filters); i++)
    if (!name || !strcmp(name, sdm_filters[i].name))
      if (sdm_filters[i].freq <= freq)
        return &sdm_filters[i];
  return NULL;
}

static double sdm_filter_calc(const double *s, double *d,
                              const sdm_filter_t *f,
                              double x, double y)
{
  const double *a = f->a;
  const double *g = f->g;
  double v;
  int i;
  
  d[0] = s[0] - g[0] * s[1] + x - y;
  v = x + a[0] * d[0];
  
  for (i = 1; i < f->order - 1; i++) {
    d[i] = s[i] + s[i - 1] - g[i] * s[i + 1];
    v += a[i] * d[i];
  }
  
  d[i] = s[i] + s[i - 1];
  v += a[i] * d[i];
  
  return v;
}

//MARK:- Trellis paths method

static void sdm_filter_calc2(sdm_state_t *src, sdm_state_t *dst,
                             const sdm_filter_t *f, double x)
{
  const double *a = f->a;
  double v;
  int i;
  
  v = sdm_filter_calc(src->state, dst[0].state, f, x, 0.0);
  
  for (i = 0; i < f->order; i++)
    dst[1].state[i] = dst[0].state[i];
  
  dst[0].state[0] += 1.0;
  dst[1].state[0] -= 1.0;
  
  dst[0].cost = src->cost + sqr(v + a[0]);
  dst[1].cost = src->cost + sqr(v - a[0]);
}

static inline unsigned sdm_histbuf_get(sdm_t *p)
{
  return p->hist_free[--p->hist_fnum];
}

static inline void sdm_histbuf_put(sdm_t *p, unsigned h)
{
  p->hist_free[p->hist_fnum++] = h;
}

static inline unsigned get_bit(uint8_t *p, unsigned i)
{
  return (p[i >> 3] >> (i & 7)) & 1;
}

static inline void put_bit(uint8_t *p, unsigned i, unsigned v)
{
  int b = p[i >> 3];
  int s = i & 7;
  b &= ~(1 << s);
  b |= v << s;
  p[i >> 3] = b;
}

static inline unsigned sdm_hist_get(sdm_t *p, unsigned h, unsigned i)
{
  return get_bit(p->hist[h], i);
}

static inline void sdm_hist_put(sdm_t *p, unsigned h, unsigned i, unsigned v)
{
  put_bit(p->hist[h], i, v);
}

static inline void sdm_hist_copy(sdm_t *p, unsigned d, unsigned s)
{
  memcpy(p->hist[d], p->hist[s], (size_t)(p->trellis_lat + 7) / 8);
}

static inline int64_t dbl2int64(double a)
{
  union { double d; int64_t i; } v;
  v.d = a;
  return v.i;
}

static inline int sdm_cmplt(sdm_state_t *a, sdm_state_t *b)
{
  //    return dbl2int64(a->cost) < dbl2int64(b->cost);
  return a->cost < b->cost;
}

static inline int sdm_cmple(sdm_state_t *a, sdm_state_t *b)
{
  return dbl2int64(a->cost) <= dbl2int64(b->cost);
  return a->cost <= b->cost;
}

static sdm_state_t *sdm_check_path(sdm_t *p, sdm_state_t *s)
{
  unsigned index = s->path & PATH_HASH_MASK;
  sdm_state_t **hash = p->path_hash;
  sdm_state_t *t = hash[index];
  
  while (t) {
    if (t->path == s->path)
      return t;
    t = t->path_list;
  }
  
  s->path_list = hash[index];
  hash[index] = s;
  
  return NULL;
}

static unsigned sdm_sort_cands(sdm_t *p, sdm_trellis_t *st)
{
  sdm_state_t *r, *s, *t;
  sdm_state_t *min;
  unsigned i, j, n;
  
  for (i = 0; i < 2 * p->num_cands; i++) {
    s = &st->sdm[i];
    p->path_hash[s->path & PATH_HASH_MASK] = NULL;
    if (!i || sdm_cmplt(s, min))
      min = s;
  }
  
  for (i = 0, n = 0; i < 2 * p->num_cands; i++) {
    s = &st->sdm[i];
    
    if (s->next != min->next)
      continue;
    
    if (n == p->trellis_num && sdm_cmple(st->act[n - 1], s))
      continue;
    
    t = sdm_check_path(p, s);
    
    if (!t) {
      for (j = n; j > 0; j--) {
        t = st->act[j - 1];
        if (sdm_cmple(t, s))
          break;
        st->act[j] = t;
      }
      if (j < p->trellis_num)
        st->act[j] = s;
      if (n < p->trellis_num)
        n++;
      continue;
    }
    
    if (sdm_cmple(t, s))
      continue;
    
    for (j = 0; j < n; j++) {
      r = st->act[j];
      if (sdm_cmple(s, r))
        break;
    }
    
    st->act[j++] = s;
    
    while (r != t && j < n) {
      sdm_state_t *u = st->act[j];
      st->act[j] = r;
      r = u;
      j++;
    }
  }
  
  return n;
}

static inline void sdm_step(sdm_t *p, sdm_state_t *cur, sdm_state_t *next,
                            double x)
{
  const sdm_filter_t *f = p->filter;
  int i;
  
  sdm_filter_calc2(cur, next, f, x);
  
  for (i = 0; i < 2; i++) {
    next[i].path = (cur->path << 1 | i) & p->trellis_mask;
    next[i].hist = cur->hist;
    next[i].next = cur->next;
    next[i].parent = cur;
  }
}

static sox_sample_t sdm_sample_trellis(sdm_t *p, double x)
{
  sdm_trellis_t *st_cur = &p->trellis[p->idx];
  sdm_trellis_t *st_next = &p->trellis[p->idx ^ 1];
  double min_cost;
  unsigned new_cands;
  unsigned next_pos;
  unsigned output;
  unsigned i;
  
  next_pos = p->pos + 1;
  if (next_pos == p->trellis_lat)
    next_pos = 0;
  
  for (i = 0; i < p->num_cands; i++) {
    sdm_state_t *cur = st_cur->act[i];
    sdm_state_t *next = &st_next->sdm[2 * i];
    sdm_step(p, cur, next, x);
    cur->next = sdm_hist_get(p, cur->hist, next_pos);
    cur->hist_used = 0;
  }
  
  new_cands = sdm_sort_cands(p, st_next);
  min_cost = st_next->act[0]->cost;
  output = st_next->act[0]->next;
  
  for (i = 0; i < new_cands; i++) {
    sdm_state_t *s = st_next->act[i];
    if (s->parent->hist_used) {
      unsigned h = sdm_histbuf_get(p);
      sdm_hist_copy(p, h, s->hist);
      s->hist = h;
    } else {
      s->parent->hist_used = 1;
    }
    
    s->cost -= min_cost;
    s->next = s->parent->next;
    sdm_hist_put(p, s->hist, p->pos, s->path & 1);
  }
  
  for (i = 0; i < p->num_cands; i++) {
    sdm_state_t *s = st_cur->act[i];
    if (!s->hist_used)
      sdm_histbuf_put(p, s->hist);
  }
  
  if (new_cands < p->num_cands)
    p->conv_fail++;
  
  p->num_cands = new_cands;
  p->pos = next_pos;
  p->idx ^= 1;
  
  return output ? SOX_SAMPLE_MAX : -SOX_SAMPLE_MAX;
}

//MARK:- Simple noise filtering

static sox_sample_t sdm_sample_1bit(sdm_t *p, double x)
{
  const sdm_filter_t *f = p->filter;
  double next[MAX_FILTER_ORDER];
  double y, v;
  
  v = sdm_filter_calc(p->simple_state, next, f, x, p->prev_y);
  y = signbit(v) ? -1.0 : 1.0;
  
  memcpy(p->simple_state, next, (size_t)f->order * sizeof(*next));
  p->prev_y = y;
  
  return y;
}

//MARK:- Processing

#define SDM_KERNEL_BEGIN()                                                   \
  const double *a = p->filter->a;                                           \
  const double *g = p->filter->g;                                           \
  double s0 = p->simple_state[0];                                           \
  double s1 = p->simple_state[1];                                           \
  double s2 = p->simple_state[2];                                           \
  double s3 = p->simple_state[3];                                           \
  double y = p->prev_y;                                                     \
  const double scale = 0.5 / SOX_SAMPLE_MAX

#define SDM_KERNEL_LOAD_HIGH_STATE()                                        \
  double s4 = p->simple_state[4];                                           \
  double s5 = p->simple_state[5];                                           \
  double s6 = p->simple_state[6];                                           \
  double s7 = p->simple_state[7]

#define SDM_KERNEL_STEP_LOW()                                                \
  double x = *ibuf++ * scale;                                               \
  double d0 = s0 - g[0] * s1 + x - y;                                      \
  double d1 = s1 + s0 - g[1] * s2;                                         \
  double d2 = s2 + s1 - g[2] * s3;                                         \
  double v = x + a[0] * d0;                                                 \
  v += a[1] * d1;                                                          \
  v += a[2] * d2

#define SDM_KERNEL_OUTPUT()                                                  \
  y = signbit(v) ? -1.0 : 1.0;                                             \
  *obuf++ = (sox_sample_t)(y * SOX_SAMPLE_MAX)

static void sdm_process_simple_order4(sdm_t *p, const sox_sample_t *ibuf,
                                      sox_sample_t *obuf, size_t len)
{
  SDM_KERNEL_BEGIN();

  while (len--) {
    SDM_KERNEL_STEP_LOW();
    double d3 = s3 + s2;
    v += a[3] * d3;
    SDM_KERNEL_OUTPUT();
    s0 = d0, s1 = d1, s2 = d2, s3 = d3;
  }

  p->simple_state[0] = s0;
  p->simple_state[1] = s1;
  p->simple_state[2] = s2;
  p->simple_state[3] = s3;
  p->prev_y = y;
}

static void sdm_process_simple_order5(sdm_t *p, const sox_sample_t *ibuf,
                                      sox_sample_t *obuf, size_t len)
{
  SDM_KERNEL_BEGIN();
  double s4 = p->simple_state[4];

  while (len--) {
    SDM_KERNEL_STEP_LOW();
    double d3 = s3 + s2 - g[3] * s4;
    double d4 = s4 + s3;
    v += a[3] * d3;
    v += a[4] * d4;
    SDM_KERNEL_OUTPUT();
    s0 = d0, s1 = d1, s2 = d2, s3 = d3, s4 = d4;
  }

  p->simple_state[0] = s0;
  p->simple_state[1] = s1;
  p->simple_state[2] = s2;
  p->simple_state[3] = s3;
  p->simple_state[4] = s4;
  p->prev_y = y;
}

static void sdm_process_simple_order6(sdm_t *p, const sox_sample_t *ibuf,
                                      sox_sample_t *obuf, size_t len)
{
  SDM_KERNEL_BEGIN();
  double s4 = p->simple_state[4];
  double s5 = p->simple_state[5];

  while (len--) {
    SDM_KERNEL_STEP_LOW();
    double d3 = s3 + s2 - g[3] * s4;
    double d4 = s4 + s3 - g[4] * s5;
    double d5 = s5 + s4;
    v += a[3] * d3;
    v += a[4] * d4;
    v += a[5] * d5;
    SDM_KERNEL_OUTPUT();
    s0 = d0, s1 = d1, s2 = d2, s3 = d3, s4 = d4, s5 = d5;
  }

  p->simple_state[0] = s0;
  p->simple_state[1] = s1;
  p->simple_state[2] = s2;
  p->simple_state[3] = s3;
  p->simple_state[4] = s4;
  p->simple_state[5] = s5;
  p->prev_y = y;
}

static void sdm_process_simple_order7(sdm_t *p, const sox_sample_t *ibuf,
                                      sox_sample_t *obuf, size_t len)
{
  SDM_KERNEL_BEGIN();
  double s4 = p->simple_state[4];
  double s5 = p->simple_state[5];
  double s6 = p->simple_state[6];

  while (len--) {
    SDM_KERNEL_STEP_LOW();
    double d3 = s3 + s2 - g[3] * s4;
    double d4 = s4 + s3 - g[4] * s5;
    double d5 = s5 + s4 - g[5] * s6;
    double d6 = s6 + s5;
    v += a[3] * d3;
    v += a[4] * d4;
    v += a[5] * d5;
    v += a[6] * d6;
    SDM_KERNEL_OUTPUT();
    s0 = d0, s1 = d1, s2 = d2, s3 = d3;
    s4 = d4, s5 = d5, s6 = d6;
  }

  p->simple_state[0] = s0;
  p->simple_state[1] = s1;
  p->simple_state[2] = s2;
  p->simple_state[3] = s3;
  p->simple_state[4] = s4;
  p->simple_state[5] = s5;
  p->simple_state[6] = s6;
  p->prev_y = y;
}

static void sdm_process_simple_order8(sdm_t *p, const sox_sample_t *ibuf,
                                      sox_sample_t *obuf, size_t len)
{
  SDM_KERNEL_BEGIN();
  SDM_KERNEL_LOAD_HIGH_STATE();

  while (len--) {
    double x = *ibuf++ * scale;
    double d0 = s0 - g[0] * s1 + x - y;
    double d1 = s1 + s0 - g[1] * s2;
    double d2 = s2 + s1 - g[2] * s3;
    double d3 = s3 + s2 - g[3] * s4;
    double d4 = s4 + s3 - g[4] * s5;
    double d5 = s5 + s4 - g[5] * s6;
    double d6 = s6 + s5 - g[6] * s7;
    double d7 = s7 + s6;
    double v01 = a[0] * d0 + a[1] * d1;
    double v23 = a[2] * d2 + a[3] * d3;
    double v45 = a[4] * d4 + a[5] * d5;
    double v67 = a[6] * d6 + a[7] * d7;
    double v = x + (v01 + v23) + (v45 + v67);
    SDM_KERNEL_OUTPUT();
    s0 = d0, s1 = d1, s2 = d2, s3 = d3;
    s4 = d4, s5 = d5, s6 = d6, s7 = d7;
  }

  p->simple_state[0] = s0;
  p->simple_state[1] = s1;
  p->simple_state[2] = s2;
  p->simple_state[3] = s3;
  p->simple_state[4] = s4;
  p->simple_state[5] = s5;
  p->simple_state[6] = s6;
  p->simple_state[7] = s7;
  p->prev_y = y;
}

#undef SDM_KERNEL_OUTPUT
#undef SDM_KERNEL_STEP_LOW
#undef SDM_KERNEL_LOAD_HIGH_STATE
#undef SDM_KERNEL_BEGIN

static void sdm_process_simple(sdm_t *p, const sox_sample_t *ibuf,
                               sox_sample_t *obuf, size_t len)
{
  switch (p->filter->order) {
    case 4: sdm_process_simple_order4(p, ibuf, obuf, len); break;
    case 5: sdm_process_simple_order5(p, ibuf, obuf, len); break;
    case 6: sdm_process_simple_order6(p, ibuf, obuf, len); break;
    case 7: sdm_process_simple_order7(p, ibuf, obuf, len); break;
    case 8: sdm_process_simple_order8(p, ibuf, obuf, len); break;
    default: assert(0);
  }
}

static sox_bool sdm_simple_state_valid(const sdm_t *p)
{
  int i;

  if (!isfinite(p->prev_y))
    return sox_false;
  for (i = 0; i < p->filter->order; ++i)
    if (!isfinite(p->simple_state[i]) ||
        fabs(p->simple_state[i]) > SDM_STATE_LIMIT)
      return sox_false;
  return sox_true;
}

static size_t sdm_process_simple_packed(const sdm_filter_t *f,
                                        double *state, double *prev_y,
                                        uint8_t *packet,
                                        unsigned packet_bits,
                                        const sox_sample_t *ibuf,
                                        sox_sample_t *obuf, size_t len,
                                        size_t input_stride,
                                        size_t output_stride)
{
  double y = *prev_y;
  const double scale = 0.5 / SOX_SAMPLE_MAX;
  size_t emitted = 0;

  if (f->order == 8) {
    const double *a = f->a;
    const double *g = f->g;
    double s0 = state[0];
    double s1 = state[1];
    double s2 = state[2];
    double s3 = state[3];
    double s4 = state[4];
    double s5 = state[5];
    double s6 = state[6];
    double s7 = state[7];

#define SDM_PACKED_ORDER8_STEP(target) do {                                 \
      double x = *ibuf * scale;                                             \
      double d0 = s0 - g[0] * s1 + x - y;                                  \
      double d1 = s1 + s0 - g[1] * s2;                                     \
      double d2 = s2 + s1 - g[2] * s3;                                     \
      double d3 = s3 + s2 - g[3] * s4;                                     \
      double d4 = s4 + s3 - g[4] * s5;                                     \
      double d5 = s5 + s4 - g[5] * s6;                                     \
      double d6 = s6 + s5 - g[6] * s7;                                     \
      double d7 = s7 + s6;                                                  \
      double v01 = a[0] * d0 + a[1] * d1;                                  \
      double v23 = a[2] * d2 + a[3] * d3;                                  \
      double v45 = a[4] * d4 + a[5] * d5;                                  \
      double v67 = a[6] * d6 + a[7] * d7;                                  \
      double v = x + (v01 + v23) + (v45 + v67);                            \
      y = signbit(v) ? -1.0 : 1.0;                                         \
      (target) = (uint8_t)(((target) << 1) | (y > 0));                      \
      s0 = d0, s1 = d1, s2 = d2, s3 = d3;                                 \
      s4 = d4, s5 = d5, s6 = d6, s7 = d7;                                 \
      ibuf += input_stride;                                                  \
    } while (0)

    while (packet_bits && len) {
      SDM_PACKED_ORDER8_STEP(*packet);
      packet_bits++;
      len--;
      if (packet_bits == 8) {
        *obuf = SOX_DSD_PACKED_BYTE(*packet, 8);
        obuf += output_stride;
        packet_bits = 0;
        *packet = 0;
        emitted++;
      }
    }

    while (len >= 8) {
      uint8_t value = 0;
      unsigned i;

      for (i = 0; i < 8; ++i)
        SDM_PACKED_ORDER8_STEP(value);
      *obuf = SOX_DSD_PACKED_BYTE(value, 8);
      obuf += output_stride;
      emitted++;
      len -= 8;
    }

    while (len--) {
      SDM_PACKED_ORDER8_STEP(*packet);
      packet_bits++;
    }

#undef SDM_PACKED_ORDER8_STEP

    state[0] = s0;
    state[1] = s1;
    state[2] = s2;
    state[3] = s3;
    state[4] = s4;
    state[5] = s5;
    state[6] = s6;
    state[7] = s7;
  } else {
    double next[MAX_FILTER_ORDER];

    while (len--) {
      double x = *ibuf * scale;
      double v = sdm_filter_calc(state, next, f, x, y);

      y = signbit(v) ? -1.0 : 1.0;
      *packet = (uint8_t)((*packet << 1) | (y > 0));
      if (++packet_bits == 8) {
        *obuf = SOX_DSD_PACKED_BYTE(*packet, 8);
        obuf += output_stride;
        packet_bits = 0;
        *packet = 0;
        emitted++;
      }
      memcpy(state, next, (size_t)f->order * sizeof(*next));
      ibuf += input_stride;
    }
  }

  *prev_y = y;
  return emitted;
}

int sdm_process(sdm_t *p, const sox_sample_t *ibuf, sox_sample_t *obuf,
                size_t *ilen, size_t *olen)
{
  sox_sample_t *out = obuf;
  size_t len = *ilen = min(*ilen, *olen);
  double x;
  
  if (p->trellis_mask) {
    if (p->pending < p->trellis_lat) {
      size_t pre = min(p->trellis_lat - p->pending, len);
      p->pending += pre;
      len -= pre;
      while (pre--) {
        x = *ibuf++ * (0.5 / SOX_SAMPLE_MAX);
        sdm_sample_trellis(p, x);
      }
    }
    while (len--) {
      x = *ibuf++ * (0.5 / SOX_SAMPLE_MAX);
      *out++ = sdm_sample_trellis(p, x);
    }
  } else {
    sdm_process_simple(p, ibuf, out, len);
    if (!sdm_simple_state_valid(p)) {
      p->failed = 1;
      *olen = 0;
      return SOX_EOF;
    }
    out += len;
  }
  
  *olen = out - obuf;
  
  return SOX_SUCCESS;
}

int sdm_drain(sdm_t *p, sox_sample_t *obuf, size_t *olen)
{
  if (p->trellis_mask) {
    size_t len = *olen = min(p->pending, *olen);
    
    if (!p->draining && p->pending < p->trellis_lat) {
      unsigned flush = p->trellis_lat - p->pending;
      while (flush--)
        sdm_sample_trellis(p, 0.0);
    }
    
    p->draining = 1;
    p->pending -= len;
    
    while (len--)
      *obuf++ = sdm_sample_trellis(p, 0.0);
  } else {
    *olen = 0;
  }
  
  return SOX_SUCCESS;
}

static inline void sdm_packet_push(sdm_t *p, uint8_t **out, int bit)
{
  p->packet = (uint8_t)((p->packet << 1) | !!bit);
  if (++p->packet_bits == 8) {
    *(*out)++ = p->packet;
    p->packet = 0;
    p->packet_bits = 0;
  }
}

///Process input in 64bit format into 8bit packets of 1bit samples
/// - important: inLength must be a multiple of 8
/// - important: outPackets size must be at least inLength / 8
/// - returns: number of packets in outPackets
size_t sdm_packet_process(sdm_t *p, const double *inSamples,
                          uint8_t *outPackets, size_t inLength)
{
  uint8_t *oPacket = outPackets;
  size_t len = inLength - inLength % 8;
  
  if (p->trellis_mask) {
    while (len--) {
      sox_sample_t sample = sdm_sample_trellis(p, *inSamples++ / 2.0);
      if (p->pending < p->trellis_lat)
        ++p->pending;
      else
        sdm_packet_push(p, &oPacket, sample > 0);
    }
  } else {
    while (len--) {
      sox_sample_t sample = sdm_sample_1bit(p, *inSamples++ / 2.0);
      sdm_packet_push(p, &oPacket, sample > 0);
    }
  }
  return oPacket - outPackets;
}

///Drain filter in 8bit packets
/// - returns: number of packets in outPackets
size_t sdm_packet_drain(sdm_t *p, uint8_t *outPackets, size_t outBufSize)
{
  uint8_t *oPacket = outPackets;
  
  if (!p->trellis_mask || !outBufSize)
    return 0;

  if (!p->draining && p->pending < p->trellis_lat) {
    unsigned flush = p->trellis_lat - p->pending;
    while (flush--)
      sdm_sample_trellis(p, 0.0);
  }

  p->draining = 1;

  {
    size_t capacity = outBufSize * 8 - p->packet_bits;
    size_t len = min(p->pending, capacity);
    p->pending -= len;

    while (len--) {
      sox_sample_t sample = sdm_sample_trellis(p, 0.0);
      sdm_packet_push(p, &oPacket, sample > 0);
    }
  }

  return oPacket - outPackets;
}

sdm_t *sdm_init(const char *filter_name,
                unsigned freq,
                unsigned trellis_order,
                unsigned trellis_num,
                unsigned trellis_latency,
                unsigned threads)
{
  sdm_t *p;
  const sdm_filter_t *f;
  sdm_trellis_t *st;
  unsigned i;
  
  if (trellis_order > SDM_TRELLIS_MAX_ORDER) {
    lsx_fail("trellis order too high (max %d)", SDM_TRELLIS_MAX_ORDER);
    return NULL;
  }
  
  if (trellis_num > SDM_TRELLIS_MAX_NUM) {
    lsx_fail("trellis size too high (max %d)", SDM_TRELLIS_MAX_NUM);
    return NULL;
  }
  
  if (trellis_latency > SDM_TRELLIS_MAX_LAT) {
    lsx_fail("trellis latency too high (max %d)", SDM_TRELLIS_MAX_LAT);
    return NULL;
  }

  if (!threads) {
#if defined HAVE_OPENMP
    threads = (unsigned)omp_get_max_threads();
#else
    threads = 1;
#endif
  }

  if (trellis_order)
    threads = 1;

  p = aligned_alloc((size_t)32, sizeof(*p));
  if (!p)
    return NULL;
  
  memset(p, 0, sizeof(*p));
  
  p->threads = threads;
  p->filter = sdm_find_filter(filter_name, freq);
  if (!p->filter) {
    lsx_fail("invalid filter name `%s'", filter_name);
    aligned_free(p);
    return NULL;
  }
  
  f = p->filter;
  st = &p->trellis[0];
  
  if (trellis_order || f->trellis_order) {
    if (trellis_order < 1)
      trellis_order = f->trellis_order ? f->trellis_order : 13;
    
    if (trellis_num)
      p->trellis_num = trellis_num;
    else
      p->trellis_num = f->trellis_num ? f->trellis_num : 8;
    
    if (trellis_latency)
      p->trellis_lat = trellis_latency;
    else
      p->trellis_lat = f->trellis_lat ? f->trellis_lat : 1024;
    
    p->trellis_mask = ((uint64_t)1 << trellis_order) - 1;
    
    for (i = 0; i < 2 * p->trellis_num; i++)
      sdm_histbuf_put(p, i);
    
    p->num_cands = 1;
    
    st->sdm[0].hist = sdm_histbuf_get(p);
    st->sdm[0].path = 0;
    st->act[0] = &st->sdm[0];
  }
  
  return p;
}

void sdm_close(sdm_t *p)
{
  if (p->conv_fail)
    lsx_warn("failed to converge %"PRId64" times", p->conv_fail);
  
  aligned_free(p);
}

//MARK:- SoX interface

typedef struct sdm_effect {
  sdm_t        **sdm;
  size_t       *emitted;
  uint8_t       *packet;
  uint8_t       packet_bits;
  const char   *filter_name;
  uint32_t      channels;
  uint32_t      threads;
  uint32_t      trellis_order;
  uint32_t      trellis_num;
  uint32_t      trellis_lat;
#if HAVE_VULKAN
  lsx_sdm_vulkan_t *vulkan;
  lsx_vulkan_context_t *vulkan_engine;
  unsigned      vulkan_rate;
  float         *vulkan_input;
  size_t        vulkan_input_frames;
  size_t        vulkan_input_capacity;
  uint8_t const *vulkan_output;
  size_t        vulkan_output_bytes;
  size_t        vulkan_output_stride;
  size_t        vulkan_output_pos;
#endif
} sdm_effect_t;

#if HAVE_VULKAN
static int consume_vulkan_resident_effect(sox_effect_t *effp, lsx_vulkan_resident_buffer_t const *input, sox_bool *input_consumed, uint64_t *input_clips, sox_sample_t *obuf, size_t *osamp, sox_bool *active);

static lsx_vulkan_effect_endpoint_t const vulkan_resident_endpoint = {
  NULL,
  NULL,
  consume_vulkan_resident_effect,
  NULL,
  NULL
};

/*
 * The logical modulator batch follows whoever feeds it, so the context is
 * built on first use: the resident producer's declared block when the chain
 * pairs up, the default host batch otherwise.  The backend owns the small
 * carry area needed for a partial FSM block between producer slices.
 */
static int ensure_vulkan_context(sdm_effect_t *p, size_t batch_frames)
{
  if (p->vulkan)
    return SOX_SUCCESS;
  p->vulkan = lsx_sdm_vulkan_create(
      p->vulkan_engine, p->vulkan_rate, p->channels, batch_frames);
  if (!p->vulkan)
    return SOX_EOF;
  return SOX_SUCCESS;
}

static int ensure_vulkan_host_input(sdm_effect_t *p)
{
  if (ensure_vulkan_context(p, 0) != SOX_SUCCESS)
    return SOX_EOF;
  if (!p->vulkan_input) {
    p->vulkan_input_capacity =
        lsx_sdm_vulkan_input_capacity(p->vulkan);
    p->vulkan_input = lsx_calloc(
        p->vulkan_input_capacity * p->channels,
        sizeof(*p->vulkan_input));
  }
  return SOX_SUCCESS;
}

static void emit_vulkan_output(sdm_effect_t *p, sox_sample_t *obuf,
                               size_t *osamp)
{
  size_t capacity = *osamp / p->channels;
  size_t remaining = p->vulkan_output_bytes - p->vulkan_output_pos;
  size_t groups = min(capacity, remaining / 4u);
  size_t channel;

  /*
   * The GPU output is planar because DSF stores channels in separate blocks.
   * A SoX sample carries one complete 32-bit DSD word in this packed mode.
   */
  for (channel = 0; channel < p->channels; ++channel)
    memcpy(obuf + channel * groups,
        p->vulkan_output +
          channel * p->vulkan_output_stride +
          p->vulkan_output_pos,
        groups * sizeof(*obuf));
  p->vulkan_output_pos += groups * 4u;
  if (p->vulkan_output_pos == p->vulkan_output_bytes) {
    p->vulkan_output = NULL;
    p->vulkan_output_bytes = 0;
    p->vulkan_output_stride = 0;
    p->vulkan_output_pos = 0;
  }
  *osamp = groups * p->channels;
}

static int process_vulkan_input(sdm_effect_t *p, size_t frames)
{
  if (lsx_sdm_vulkan_process(
      p->vulkan, p->vulkan_input, frames,
      &p->vulkan_output, &p->vulkan_output_bytes,
      &p->vulkan_output_stride) != SOX_SUCCESS)
    return SOX_EOF;
  if (p->vulkan_output_bytes % 4u ||
      p->vulkan_output_stride % 4u) {
    lsx_fail("Vulkan DSD output is not word aligned");
    return SOX_EOF;
  }
  p->vulkan_output_pos = 0;
  return SOX_SUCCESS;
}

static int consume_vulkan_resident_effect(sox_effect_t *effp, lsx_vulkan_resident_buffer_t const *input, sox_bool *input_consumed, uint64_t *input_clips, sox_sample_t *obuf, size_t *osamp, sox_bool *active)
{
  sdm_effect_t *p = effp->priv;
  sox_bool output_ready;

  *input_consumed = sox_false;
  *input_clips = 0;
  if (input && ensure_vulkan_context(
      p, max(input->block_elements, input->valid_elements)) !=
      SOX_SUCCESS) {
    *osamp = 0;
    return SOX_EINVAL;
  }
  if (!p->vulkan) {
    *osamp = 0;
    *active = sox_false;
    return SOX_SUCCESS;
  }
  if (p->vulkan_output) {
    emit_vulkan_output(p, obuf, osamp);
    *active = p->vulkan_output ||
        lsx_sdm_vulkan_resident_active(p->vulkan);
    *input_clips =
        lsx_sdm_vulkan_resident_clips(p->vulkan);
    return SOX_SUCCESS;
  }
  if (lsx_sdm_vulkan_consume_resident(
      p->vulkan, input, input_consumed, &output_ready,
      &p->vulkan_output, &p->vulkan_output_bytes,
      &p->vulkan_output_stride) != SOX_SUCCESS) {
    *osamp = 0;
    return SOX_EINVAL;
  }
  p->vulkan_output_pos = 0;
  if (output_ready &&
      (p->vulkan_output_bytes % 4u ||
      p->vulkan_output_stride % 4u)) {
    lsx_fail("resident Vulkan DSD output is not word aligned");
    *osamp = 0;
    return SOX_EOF;
  }
  if (p->vulkan_output)
    emit_vulkan_output(p, obuf, osamp);
  else
    *osamp = 0;
  *active = p->vulkan_output ||
      lsx_sdm_vulkan_resident_active(p->vulkan);
  *input_clips =
      lsx_sdm_vulkan_resident_clips(p->vulkan);
  return SOX_SUCCESS;
}

static int flow_vulkan(sdm_effect_t *p,
    sox_sample_t const *ibuf, sox_sample_t *obuf,
    size_t *isamp, size_t *osamp)
{
  size_t input_frames = *isamp / p->channels;
  size_t room;
  size_t consumed;
  size_t frame;
  size_t channel;
  size_t core_frames;

  /*
   * A priming call with nothing in it must not build the context: the
   * batch belongs to whoever feeds the modulator, and a resident producer
   * may still be about to claim it.
   */
  if (!p->vulkan && !input_frames && !p->vulkan_output) {
    *isamp = 0;
    *osamp = 0;
    return SOX_SUCCESS;
  }
  if (ensure_vulkan_host_input(p) != SOX_SUCCESS) {
    *isamp = 0;
    *osamp = 0;
    return SOX_EOF;
  }
  core_frames = lsx_sdm_vulkan_input_capacity(p->vulkan);
  if (p->vulkan_output) {
    *isamp = 0;
    emit_vulkan_output(p, obuf, osamp);
    return SOX_SUCCESS;
  }
  room = p->vulkan_input_capacity - p->vulkan_input_frames;
  consumed = min(input_frames, room);
  /* SoX input is interleaved; the backend transposes it after chunking. */
  for (frame = 0; frame < consumed; ++frame)
    for (channel = 0; channel < p->channels; ++channel)
      p->vulkan_input[
          (p->vulkan_input_frames + frame) * p->channels +
          channel] = SOX_SAMPLE_TO_FLOAT_32BIT(
              ibuf[frame * p->channels + channel], effp->clips);
  p->vulkan_input_frames += consumed;
  *isamp = consumed * p->channels;
  if (p->vulkan_input_frames < p->vulkan_input_capacity) {
    *osamp = 0;
    return SOX_SUCCESS;
  }
  if (process_vulkan_input(p, core_frames) != SOX_SUCCESS) {
    *osamp = 0;
    return SOX_EOF;
  }
  p->vulkan_input_frames = 0;
  emit_vulkan_output(p, obuf, osamp);
  return SOX_SUCCESS;
}

static int drain_vulkan(sdm_effect_t *p, sox_sample_t *obuf, size_t *osamp)
{
  if (!p->vulkan) {
    *osamp = 0;
    return SOX_SUCCESS;
  }
  if (p->vulkan_output) {
    emit_vulkan_output(p, obuf, osamp);
    return SOX_SUCCESS;
  }
  if (p->vulkan_input_frames) {
    size_t frames = p->vulkan_input_frames;

    if (process_vulkan_input(p, frames) != SOX_SUCCESS) {
      *osamp = 0;
      return SOX_EOF;
    }
    p->vulkan_input_frames = 0;
    emit_vulkan_output(p, obuf, osamp);
    return SOX_SUCCESS;
  }
  *osamp = 0;
  return SOX_SUCCESS;
}
#endif

static void sdm_effect_cleanup(sdm_effect_t *p)
{
  unsigned channel;
  size_t state_count = p->channels;

#if HAVE_VULKAN
  lsx_sdm_vulkan_destroy(p->vulkan);
  free(p->vulkan_input);
  p->vulkan = NULL;
  p->vulkan_input = NULL;
#endif
  for (channel = 0; channel < state_count; ++channel)
    if (p->sdm && p->sdm[channel])
      sdm_close(p->sdm[channel]);
  free(p->emitted);
  free(p->packet);
  free(p->sdm);
  p->emitted = NULL;
  p->packet = NULL;
  p->sdm = NULL;
}

static int getopts(sox_effect_t *effp, int argc, char **argv)
{
  sdm_effect_t *p = effp->priv;
  lsx_getopt_t optstate;
  int c;

  lsx_getopt_init(argc, argv, "+f:t:n:l:", NULL, lsx_getopt_flag_none,
                  1, &optstate);

  while ((c = lsx_getopt(&optstate)) != -1) switch (c) {
    case 'f': p->filter_name = optstate.arg; break;
      GETOPT_NUMERIC(optstate, 't', trellis_order, 3, SDM_TRELLIS_MAX_ORDER)
      GETOPT_NUMERIC(optstate, 'n', trellis_num, 4, SDM_TRELLIS_MAX_NUM)
      GETOPT_NUMERIC(optstate, 'l', trellis_lat, 100, SDM_TRELLIS_MAX_LAT)
    default: lsx_fail("invalid option `-%c'", optstate.opt); return lsx_usage(effp);
  }

  return argc != optstate.ind ? lsx_usage(effp) : SOX_SUCCESS;
}

static int start(sox_effect_t *effp)
{
  sdm_effect_t *p = effp->priv;
  sox_rate_t sdm_rate = effp->in_signal.rate;
  unsigned channel;

  p->channels = effp->in_signal.channels;
#if HAVE_VULKAN
  if (sox_globals.vulkan_profile != sox_vulkan_profile_none) {
    lsx_vulkan_context_t *vulkan;

    /*
     * The modulator does not resample: it consumes whatever arrives at the
     * DSD rate, so the chain must carry a rate effect ahead of it.
     */
    if (sdm_rate > UINT_MAX || sdm_rate != (unsigned)sdm_rate) {
      lsx_fail("Vulkan SDM requires an integer input sample rate");
      return SOX_EOF;
    }
    if (!lsx_sdm_vulkan_dsd_rate_supported((unsigned)sdm_rate)) {
      lsx_fail(
          "Vulkan SDM needs its input already at a DSD rate; put a rate "
          "effect ahead of it, for instance `rate 2822400' for DSD64 "
          "(DSD64..DSD1024 is 2822400 to 45158400)");
      return SOX_EOF;
    }
    if (p->filter_name || p->trellis_order || p->trellis_num ||
        p->trellis_lat)
      lsx_warn(
          "Vulkan SDM uses the conservative MASH-2/FSM; "
          "-f, -t, -n and -l are ignored");
    vulkan = lsx_vulkan_context_get(effp->global_info);
    if (!vulkan)
      return SOX_EOF;
    p->vulkan_engine = vulkan;
    p->vulkan_rate = (unsigned)sdm_rate;
    if (!getenv("SOX_VULKAN_DISABLE_RESIDENT_EFFECT_BOUNDARY"))
      effp->internal_chain_endpoint = &vulkan_resident_endpoint;
    effp->out_signal.precision = 1;
    effp->out_signal.rate = sdm_rate;
    effp->out_signal.packing = SOX_DSD_PACKING_WORD;
    return SOX_SUCCESS;
  }
#endif
  p->sdm = lsx_calloc(p->channels, sizeof(*p->sdm));
  p->emitted = lsx_calloc(p->channels, sizeof(*p->emitted));

  for (channel = 0; channel < p->channels; ++channel) {
    p->sdm[channel] = sdm_init(
        p->filter_name, (unsigned)sdm_rate, p->trellis_order,
        p->trellis_num, p->trellis_lat, p->threads);
    if (!p->sdm[channel]) {
      sdm_effect_cleanup(p);
      return SOX_EOF;
    }
  }

  p->threads = p->sdm[0]->threads;
  p->packet = lsx_calloc(p->channels, sizeof(*p->packet));
  effp->out_signal.precision = 1;
  effp->out_signal.rate = sdm_rate;
  if (!p->sdm[0]->trellis_mask)
    effp->out_signal.packing = SOX_DSD_PACKING_BYTE;

  return SOX_SUCCESS;
}

static int flow(sox_effect_t *effp, const sox_sample_t *ibuf,
                sox_sample_t *obuf, size_t *isamp, size_t *osamp)
{
  sdm_effect_t *p = effp->priv;
  const size_t channels = p->channels;
  size_t wide;
  sdm_t *first;
  ptrdiff_t job;

#if HAVE_VULKAN
  if (p->vulkan_engine)
    return flow_vulkan(p, ibuf, obuf, isamp, osamp);
#endif
  first = p->sdm[0];
  if (first->trellis_mask) {
    wide = min(*isamp, *osamp) / channels;
    *isamp = wide * channels;
    size_t pre = first->pending < first->trellis_lat ?
        min(first->trellis_lat - first->pending, wide) : 0;
#if defined HAVE_OPENMP
    int thread_count = (int)min(
        channels, (size_t)omp_get_max_threads());
    #pragma omp parallel for \
        if(sox_globals.use_threads && thread_count > 1) \
        num_threads(thread_count) schedule(static)
#endif
    for (job = 0; job < (ptrdiff_t)channels; ++job) {
      sdm_t *sdm = p->sdm[job];
      size_t i;

      for (i = 0; i < pre; ++i)
        sdm_sample_trellis(sdm, ibuf[i * channels + job] *
                           (0.5 / SOX_SAMPLE_MAX));
      sdm->pending += pre;

      for (i = pre; i < wide; ++i)
        obuf[(i - pre) * channels + job] = sdm_sample_trellis(
            sdm, ibuf[i * channels + job] * (0.5 / SOX_SAMPLE_MAX));
    }
    *osamp = (wide - pre) * channels;
  } else {
    size_t groups = *osamp / channels;
    size_t max_frames = groups * 8;
    size_t emitted;

    if (max_frames < p->packet_bits) {
      *isamp = *osamp = 0;
      return SOX_SUCCESS;
    }

    max_frames -= p->packet_bits;
    wide = min(*isamp / channels, max_frames);
    emitted = (p->packet_bits + wide) / 8;
    *isamp = wide * channels;
#if defined HAVE_OPENMP
    int thread_count = (int)min(
        min(channels, (size_t)p->threads),
        (size_t)omp_get_max_threads());
    #pragma omp parallel for \
        if(sox_globals.use_threads && thread_count > 1) \
        num_threads(thread_count) schedule(static)
#endif
    for (job = 0; job < (ptrdiff_t)channels; ++job) {
      sdm_t *sdm = p->sdm[job];
      size_t channel_emitted = sdm_process_simple_packed(
          sdm->filter, sdm->simple_state, &sdm->prev_y,
          p->packet + job, p->packet_bits, ibuf + job, obuf + job,
          wide, channels, channels);
      p->emitted[job] = channel_emitted;
      if (!sdm_simple_state_valid(sdm))
        sdm->failed = 1;
    }
    for (job = 0; job < (ptrdiff_t)channels; ++job) {
      if (p->sdm[job]->failed) {
        *osamp = 0;
        lsx_fail("SDM diverged on channel %td", job + 1);
        return SOX_ECONVERGE;
      }
      if (p->emitted[job] != emitted) {
        *osamp = 0;
        lsx_fail("SDM channels packed asymmetrically");
        return SOX_EOF;
      }
    }
    p->packet_bits = (uint8_t)((p->packet_bits + wide) & 7);
    *osamp = emitted * channels;
  }

  return SOX_SUCCESS;
}

static int drain(sox_effect_t *effp, sox_sample_t *obuf, size_t *osamp)
{
  sdm_effect_t *p = effp->priv;
  const size_t channels = p->channels;
  sdm_t *first;
  size_t len;
  ptrdiff_t channel;

#if HAVE_VULKAN
  if (p->vulkan_engine)
    return drain_vulkan(p, obuf, osamp);
#endif
  first = p->sdm[0];
  if (!first->trellis_mask) {
    size_t channels = p->channels;
    size_t channel;

    if (!p->packet_bits || *osamp < channels) {
      *osamp = 0;
      return SOX_SUCCESS;
    }

    for (channel = 0; channel < channels; ++channel)
      obuf[channel] = SOX_DSD_PACKED_BYTE(
          p->packet[channel] << (8 - p->packet_bits),
          p->packet_bits);
    p->packet_bits = 0;
    memset(p->packet, 0, channels);
    *osamp = channels;
    return SOX_SUCCESS;
  }

  if (!first->draining && first->pending < first->trellis_lat) {
    unsigned flush = first->trellis_lat - first->pending;

    for (channel = 0; channel < (ptrdiff_t)channels; ++channel) {
      unsigned count = flush;
      while (count--)
        sdm_sample_trellis(p->sdm[channel], 0.0);
    }
  }

  len = min(first->pending, *osamp / channels);
  for (channel = 0; channel < (ptrdiff_t)channels; ++channel) {
    sdm_t *sdm = p->sdm[channel];
    size_t i;

    sdm->draining = 1;
    sdm->pending -= len;
    for (i = 0; i < len; ++i)
      obuf[i * channels + channel] = sdm_sample_trellis(sdm, 0.0);
  }

  *osamp = len * channels;
  return SOX_SUCCESS;
}

static int stop(sox_effect_t *effp)
{
  sdm_effect_t *p = effp->priv;

  effp->internal_chain_endpoint = NULL;
  sdm_effect_cleanup(p);
  return SOX_SUCCESS;
}

const sox_effect_handler_t *lsx_sdm_effect_fn(void)
{
  static sox_effect_handler_t handler = {
    "sdm", "[-f filter] [-t order] [-n num] [-l latency]"
    "\n  -f       Noise-shaping filter:"
    "\n           sdm-4..sdm-8, clans-4..clans-8"
    "\n           All orders: DSD64 through DSD1024"
    "\n           DSD rates use the 44.1-kHz family"
    "\n           sdm modulates at its input rate; put a rate effect"
    "\n           ahead of it to reach the DSD rate"
    "\n           Advanced options:"
    "\n  -t       Override trellis order"
    "\n  -n       Override number of trellis paths"
    "\n  -l       Override trellis latency",
    SOX_EFF_PREC | SOX_EFF_RATE | SOX_EFF_MCHAN,
    getopts, start, flow, drain, stop, 0, sizeof(sdm_effect_t),
  };
  return &handler;
}
