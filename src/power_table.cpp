#include "power_table.h"
#include <cstring>
#include <cctype>

namespace PowerTable
{
    // CPU TDP table keyed by "family_model" string from /proc/cpuinfo (decimal).
    // One key covers all SKUs with that family/model, so the MAXIMUM TDP of the
    // series is stored (never underestimate - the line would clip at the top).
    // Intel from Core 2 Quad Q6600 (family 6, model 0x0F) onward,
    // AMD from Phenom onward.
    // NB: every key must be unique - the first match wins.
    static const struct { const char* key; uint32_t tdp; } kCpuTdp[] =
    {
        // ---- Intel Core 2 (family 6) ----
        { "6_15",   130 },  // 0x0F Conroe: Q6600 95W, QX6850 130W (max)
        { "6_23",   136 },  // 0x17 Penryn/Yorkfield: Q9550 95W, QX9770 136W (max)

        // ---- Intel Nehalem/Westmere (family 6) ----
        { "6_26",   95  },  // 0x1A Lynnfield: i7-8xx, Xeon X34xx
        { "6_30",   130 },  // 0x1E Bloomfield: i7-9xx
        { "6_44",   130 },  // 0x2C Gulftown: i7-980X, Xeon W36xx

        // ---- Intel Sandy/Ivy Bridge (family 6) ----
        { "6_42",   95  },  // 0x2A Sandy Bridge: i7-2600K/2700K
        { "6_45",   130 },  // 0x2D Sandy Bridge-E/EP: i7-3820, Xeon E5 v1
        { "6_58",   77  },  // 0x3A Ivy Bridge: i7-3770K
        { "6_62",   135 },  // 0x3E Ivy Bridge-EP: Xeon E5 v2 (E5-2699 v2)

        // ---- Intel Haswell/Broadwell (family 6) ----
        { "6_60",   88  },  // 0x3C Haswell-S: i7-4790K
        { "6_63",   145 },  // 0x3F Haswell-E/EP: i7-5960X 140W, Xeon E5 v3 145W
        { "6_61",   47  },  // 0x3D Broadwell: i7-5500U/5700HQ (max 47W)
        { "6_79",   150 },  // 0x4F Broadwell-EP: Xeon E5-26xx v4 (E5-2696 v4)

        // ---- Intel Skylake/Kaby/Coffee (family 6) ----
        { "6_78",   45  },  // 0x4E Skylake-U/H: i7-6820HQ (max 45W)
        { "6_94",   91  },  // 0x5E Skylake-S: i7-6700K
        { "6_142",  45  },  // 0x8E Kaby/Coffee/Amber Lake-H: i7-7820HQ (max)
        { "6_158",  95  },  // 0x9E Kaby/Coffee Lake-S/R: i7-7700K/8700K/9700K

        // ---- Intel Comet/Rocket Lake (family 6) ----
        { "6_165",  125 },  // 0xA5 Comet Lake: i7-10700K
        { "6_167",  125 },  // 0xA7 Rocket Lake: i7-11700K

        // ---- Intel Alder/Raptor Lake (family 6) ----
        { "6_151",  125 },  // 0x97 Alder Lake-S: i7/i9-12700K/12900K
        { "6_183",  125 },  // 0xB7 Raptor Lake-S: i7/i9-13700K/13900K/14900K

        // ---- Intel Xeon Scalable (family 6) ----
        { "6_85",   205 },  // 0x55 Skylake/Cascade Lake-SP: Platinum 8180
        { "6_143",  350 },  // 0x8F Sapphire/Emerald Rapids: Platinum 8490H

        // ---- AMD Phenom/FX (family 16/21) ----
        { "16_3",   125 },  // Phenom II X4 955 / X6 1100T
        { "21_0",   125 },  // FX-8350 (Piledriver)
        { "21_2",   220 },  // FX-9590 (Vishera)

        // ---- AMD Ryzen (family 17 = Zen/Zen+/Zen2, 25 = Zen3/Zen4, 26 = Zen5) ----
        { "17_1",   180 },  // Ryzen 7 1800X 95W, Threadripper 1950X 180W (max)
        { "17_8",   105 },  // Ryzen 7 2700X (Zen+)
        { "17_113", 105 },  // Ryzen 7/9 3xxx (Zen 2 Matisse)
        { "25_33",  105 },  // Ryzen 5/7/9 5xxx (Zen 3 Vermeer)
        { "25_97",  170 },  // Ryzen 7/9 7xxx (Zen 4: 7700X 105W, 7950X 170W)
        { "26_68",  170 },  // Ryzen 7/9 9xxx (Zen 5: 9700X 65W, 9950X 170W)

        // Fallback
        { nullptr,   125 }
    };

    // NVIDIA GPU TDP table keyed by name substring (case-insensitive).
    // Covers GeForce from 8800 GT (2007) to RTX 50 series (2025).
    // IMPORTANT: ContainsNoCase() finds the FIRST substring match, so longer
    // patterns MUST come before shorter ones ("RTX 5060 Ti" before "RTX 5060").
    static const struct { const char* name; uint32_t tdp; } kGpuTdp[] =
    {
        // ---- RTX 40/50 series: SUPER and Ti variants first ----
        { "RTX 4070 Ti SUPER", 285 },
        { "RTX 4080 SUPER",    320 },
        { "RTX 4070 Ti",       285 },
        { "RTX 4060 Ti",       165 },
        { "RTX 5060 Ti",       180 },
        { "RTX 5070 Ti",       300 },
        { "RTX 5090 Laptop",   175 },
        { "RTX 5080 Laptop",   175 },
        { "RTX 5070 Laptop",   115 },
        { "RTX 5060 Laptop",   115 },
        { "RTX 4090 Laptop",   175 },
        { "RTX 4070 Laptop",   140 },
        { "RTX 4060 Laptop",   115 },
        { "RTX 4090",          450 },
        { "RTX 4080",          320 },
        { "RTX 4070",          200 },
        { "RTX 4060",          115 },
        { "RTX 5090",          575 },
        { "RTX 5080",          360 },
        { "RTX 5070",          250 },
        { "RTX 5060",          145 },

        // ---- RTX 30 series: Ti/Laptop first ----
        { "RTX 3090 Ti",       450 },
        { "RTX 3080 Ti",       350 },
        { "RTX 3070 Ti",       290 },
        { "RTX 3060 Ti",       200 },
        { "RTX 3050 Laptop",   75  },
        { "RTX 3060 Laptop",   115 },
        { "RTX 3070 Laptop",   125 },
        { "RTX 3080 Laptop",   150 },
        { "RTX 3090 Laptop",   150 },
        { "RTX 3090",          350 },
        { "RTX 3080",          320 },
        { "RTX 3070",          220 },
        { "RTX 3060",          170 },
        { "RTX 3050",          130 },

        // ---- RTX 20 series ----
        { "RTX 2080 Ti",       250 },
        { "RTX 2080 SUPER",    215 },
        { "RTX 2070 SUPER",    215 },
        { "RTX 2060 SUPER",    175 },
        { "RTX 2060 Laptop",   80  },
        { "RTX 2070 Laptop",   115 },
        { "RTX 2080 Laptop",   150 },
        { "RTX 2080",          215 },
        { "RTX 2070",          175 },
        { "RTX 2060",          160 },

        // ---- GTX 10 series ----
        { "GTX 1080 Ti",       250 },
        { "GTX 1070 Ti",       180 },
        { "GTX 1050 Ti",       75  },
        { "GTX 1050 Laptop",   75  },
        { "GTX 1060 Laptop",   80  },
        { "GTX 1070 Laptop",   120 },
        { "GTX 1080 Laptop",   150 },
        { "GTX 1080",          180 },
        { "GTX 1070",          150 },
        { "GTX 1060",          120 },
        { "GTX 1050",          75  },
        { "GT 1030",           30  },

        // ---- GTX 16 series (Super/Ti variants first) ----
        { "GTX 1660 Super",    125 },
        { "GTX 1660 Ti",       120 },
        { "GTX 1660",          120 },
        { "GTX 1650 Super",    100 },
        { "GTX 1650",          75  },
        { "GTX 1630",          75  },

        // ---- GTX 900 series ----
        { "GTX 980 Ti",        250 },
        { "GTX 965M",          60  },
        { "GTX 960M",          75  },
        { "GTX 950M",          75  },
        { "GTX 980",           165 },
        { "GTX 970",           145 },
        { "GTX 960",           120 },
        { "GTX 950",           90  },

        // ---- GTX 700 series ----
        { "GTX 780 Ti",        250 },
        { "GTX 750 Ti",        60  },
        { "GTX 765M",          75  },
        { "GTX 760M",          55  },
        { "GTX 780M",          100 },
        { "GTX 780",           250 },
        { "GTX 770",           230 },
        { "GTX 760",           170 },
        { "GTX 750",           55  },
        { "GTX 745",           55  },

        // ---- GTX 600 series ----
        { "GTX 660 Ti",        150 },
        { "GTX 650",           64  },
        { "GTX 690",           300 },
        { "GTX 680",           195 },
        { "GTX 670",           170 },
        { "GTX 660",           140 },

        // ---- GTX 500 series ----
        { "GTX 560 Ti",        170 },
        { "GTX 550 Ti",        116 },
        { "GTX 590",           365 },
        { "GTX 580",           244 },
        { "GTX 570",           219 },
        { "GTX 560",           150 },

        // ---- GTX 400 series ----
        { "GTX 480",           250 },
        { "GTX 470",           215 },
        { "GTX 460",           160 },

        // ---- GTX 200 series ----
        { "GTX 295",           283 },
        { "GTX 285",           204 },
        { "GTX 280",           236 },
        { "GTX 275",           219 },
        { "GTX 260",           182 },

        // ---- TITAN (only the dual-GPU Z differs) ----
        { "TITAN Z",           375 },
        { "TITAN",             250 },

        // ---- GeForce 8/9 series ----
        { "9800 GX2",          256 },
        { "9800 GTX",          140 },
        { "9600 GT",           95  },
        { "8800 GTX",          150 },
        { "8800 GTS",          144 },
        { "8800 GT",           105 },

        // ---- Low-end GT 500/600/700 (no nvmlDeviceGetPowerManagementLimit
        // on these cards, so the static TDP is the only source).
        // NB: this group must stay AFTER "GeForce 8/9 series" - otherwise the
        // "9800 GT" pattern would shadow "9800 GTX" (140 W) and match it first.
        { "GT 740",            64  },
        { "GT 730",            49  },
        { "GT 720",            19  },
        { "GT 710",            19  },
        { "GT 640",            65  },
        { "GT 635",            35  },
        { "GT 630",            65  },
        { "GT 620",            49  },
        { "GT 610",            29  },
        { "GT 520",            29  },

        // ---- Legacy low-end GeForce (2009-2010, TDP from NVIDIA specs) ----
        { "GeForce 210",       30  },
        { "8400 GS",           40  },
        { "9500 GT",           50  },
        { "9600 GSO",          84  },
        { "9800 GT",           125 },
        { "GTS 450",           106 },
        { "GTS 250",           150 },

        // Fallback
        { nullptr,             0 }
    };

    // ---- NVIDIA Professional / Datacenter (added after consumer GPUs) ----
    // These are queried first because their names contain "RTX"/"Tesla"/"A"/"H"
    // which could shadow shorter consumer matches if placed after.
    static const struct { const char* name; uint32_t tdp; } kServerGpuTdp[] =
    {
        // ---- H-series (Hopper, 2023+) ----
        { "H200",              700 },
        { "H100 80GB",         700 },
        { "H100",              700 },
        { "H100 NVL",          700 },

        // ---- A-series (Ampere datacenter) ----
        // Longer / more specific names MUST come before shorter ones because
        // ContainsNoCase does substring matching: "RTX A4000" contains "A40",
        // "RTX A2000" contains "A2". If the short pattern is checked first,
        // the RTX card gets the wrong (datacenter) TDP.
        { "RTX A6000",         300 },
        { "A6000",             300 },
        { "RTX A4000",         140 },
        { "A40",               300 },
        { "A100 80GB",         300 },
        { "A100 40GB",         250 },
        { "A100",              300 },
        { "A30",               200 },
        { "A10",               175 },
        { "RTX A2000 12GB",    70  },
        { "RTX A2000",         70  },
        { "A2",                75  },

        // ---- RTX professional (Ada) ----
        { "RTX 6000 Ada",      300 },
        { "RTX 4000 SFF Ada",  180 },
        { "RTX 4000 Ada",      140 },
        { "RTX A5000",         230 },

        // ---- Tesla (Volta) ----
        { "Tesla V100 32GB",   300 },
        { "Tesla V100 16GB",   250 },
        { "Tesla V100",        300 },
        { "V100 32GB",         300 },
        { "V100 16GB",         250 },
        { "V100",              300 },

        // ---- Tesla (Turing) ----
        { "Tesla T4",          70  },
        { "T4",                70  },

        // ---- Quadro (legacy professional) ----
        { "Quadro RTX 8000",   295 },
        { "Quadro RTX 6000",   295 },
        { "Quadro RTX 5000",   230 },
        { "Quadro RTX 4000",   130 },
        { "Quadro P6000",      250 },
        { "Quadro P5000",      180 },
        { "Quadro P4000",      75  },

        // ---- Tesla (Pascal) ----
        { "Tesla P40",         250 },
        { "Tesla P4",          75  },
        { "Tesla P100 SXM2",   300 },
        { "Tesla P100 PCIe",   250 },
        { "Tesla P100",        300 },
        { "P40",               250 },
        { "P4",                75  },
        { "P100",              300 },

        // ---- Tesla (Maxwell) ----
        { "Tesla M60",         375 },
        { "Tesla M4",          75  },
        { "Tesla M40 24GB",    300 },
        { "Tesla M40 12GB",    250 },
        { "M60",               375 },
        { "M40",               300 },

        // ---- Tesla (Kepler) ----
        { "Tesla K80",         375 },
        { "Tesla K40m",        235 },
        { "Tesla K40s",        235 },
        { "Tesla K40",         235 },
        { "Tesla K20X",        225 },
        { "Tesla K20M",        225 },
        { "K80",               375 },
        { "K40m",              235 },
        { "K40s",              235 },
        { "K40",               235 },
        { "K20X",              225 },
        { "K20M",              225 },

        // Fallback
        { nullptr,             0 }
    };

    // Case-insensitive substring search (ASCII only, GPU/CPU names are ASCII).
    static bool ContainsNoCase(const char* haystack, const char* needle)
    {
        if (!haystack || !needle) return false;
        size_t nlen = strlen(needle);
        if (nlen == 0) return false;
        for (const char* h = haystack; *h; ++h)
        {
            size_t i = 0;
            while (i < nlen && h[i] &&
                   tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i]))
                ++i;
            if (i == nlen) return true;
        }
        return false;
    }

    uint32_t GetCpuTdp(const char* familyModel)
    {
        if (!familyModel) return 125;
        for (int i = 0; kCpuTdp[i].key; ++i)
        {
            if (strcmp(kCpuTdp[i].key, familyModel) == 0)
                return kCpuTdp[i].tdp;
        }
        return 125; // conservative default for unknown CPU
    }

    uint32_t GetCpuTdpByName(const std::string& name)
    {
        // Windows: match by CPU name substring (case-insensitive).
        // Real names look like "Intel(R) Xeon(R) CPU E5-2696 v4 @ 2.20GHz",
        // "Intel(R) Core(TM)2 Quad CPU Q6600 @ 2.40GHz" or
        // "AMD FX(tm)-8350 Eight-Core Processor" - so patterns must match the
        // actual text (no "Xeon "/"Core 2 " prefixes, "(tm)" interrupts FX).
        // Longer/more specific patterns must come first ("i7-4790K" before
        // "i7-4790"), because the first substring match wins.
        static const struct { const char* pattern; uint32_t tdp; } kNameTdp[] =
        {
            // Intel Core 2 Quad (by model number)
            { "Q6600",   95  },
            { "Q9",      95  },
            { "Q8",      95  },
            // Intel Core i7 (K variant first, then base)
            { "i7-4790K",  88  }, { "i7-4790",  84  },
            { "i7-4770K",  84  }, { "i7-4770",  84  },
            { "i7-3770K",  77  }, { "i7-3770",  77  },
            { "i7-6700K",  91  }, { "i7-6700",  65  },
            { "i7-7700K",  91  }, { "i7-7700",  65  },
            { "i7-8700K",  95  }, { "i7-8700",  65  },
            { "i7-9700K",  95  }, { "i7-9700",  65  },
            { "i7-10700K", 125 }, { "i7-10700", 65  },
            { "i7-11700K", 125 }, { "i7-11700", 65  },
            { "i7-12700K", 125 }, { "i7-12700", 65  },
            { "i7-13700K", 125 }, { "i7-13700", 65  },
            { "i7-14700K", 125 }, { "i7-14700", 65  },
            // Intel Core i9
            { "i9-13900KS", 150 }, { "i9-13900K", 125 }, { "i9-13900", 65 },
            { "i9-14900KS", 150 }, { "i9-14900K", 125 }, { "i9-14900", 65 },
            { "i9-12900K", 125 }, { "i9-12900", 65  },
            // Intel Core i5
            { "i5-13600K", 125 }, { "i5-13600", 65  },
            { "i5-14600K", 125 }, { "i5-14600", 65  },
            // Intel Xeon E5 (common workstation, match by model number only)
            { "E5-2696", 150 }, { "E5-2699", 145 },
            { "E5-2697", 145 }, { "E5-2690", 135 },
            { "E5-2689", 115 }, { "E5-2687", 130 },
            { "E5-2680", 130 }, { "E5-2670", 130 },
            { "E5-2660", 130 }, { "E5-2650", 130 },
            { "E5-2640", 130 }, { "E5-2630", 130 },
            { "E5-2620", 130 }, { "E5-2603", 130 },
            // AMD (FX names contain "(tm)-" between the series and the number)
            { "9590", 220 }, { "9370", 220 },
            { "8350", 125 }, { "8370", 125 },
            { "X4 955", 125 },
            { "Ryzen 9 7950X", 170 }, { "Ryzen 9 7900X", 170 },
            { "Ryzen 9 9950X", 170 }, { "Ryzen 9 9900X", 170 },
            { "Ryzen 7 7700X", 105 }, { "Ryzen 7 7700", 65  },
            { "Ryzen 7 5800X", 105 }, { "Ryzen 7 5700X", 65  },
            { "Ryzen 7 3800X", 105 }, { "Ryzen 7 3700X", 65  },
            { "Ryzen 7 2700X", 105 }, { "Ryzen 7 2700", 65  },
            { "Ryzen 7 1800X", 95  }, { "Ryzen 7 1700X", 95  },
            { nullptr, 0 }
        };
        for (int i = 0; kNameTdp[i].pattern; ++i)
        {
            if (ContainsNoCase(name.c_str(), kNameTdp[i].pattern))
                return kNameTdp[i].tdp;
        }
        return 125; // conservative default for unknown CPU
    }

    uint32_t GetGpuTdp(const std::string& gpuName)
    {
        if (gpuName.empty()) return 0;
        for (int i = 0; kGpuTdp[i].name; ++i)
        {
            if (ContainsNoCase(gpuName.c_str(), kGpuTdp[i].name))
                return kGpuTdp[i].tdp;
        }
        // Server / professional GPUs not in the consumer table.
        for (int i = 0; kServerGpuTdp[i].name; ++i)
        {
            if (ContainsNoCase(gpuName.c_str(), kServerGpuTdp[i].name))
                return kServerGpuTdp[i].tdp;
        }
        return 0; // not found - caller uses NVML limit or estimate
    }

    double EstimateCpuPowerWatts(double loadPercent, uint32_t tdpWatts,
                                 uint32_t physicalCores)
    {
        // Unknown CPU -> conservative default, matches GetCpuTdp() fallback.
        if (tdpWatts == 0)
            tdpWatts = 125;

        const double tdp = static_cast<double>(tdpWatts);

        // Idle floor: static leakage grows with core count (~1.5 W/core) but
        // never exceeds 30% of TDP; unknown core count -> 25% of TDP.
        double idle;
        if (physicalCores > 0)
        {
            const double byCap    = 0.30 * tdp;
            const double byCores  = 1.5 * static_cast<double>(physicalCores);
            idle = (byCap < byCores) ? byCap : byCores;
        }
        else
        {
            idle = 0.25 * tdp;
        }
        if (idle >= tdp)
            idle = 0.25 * tdp;

        // Clamp load (NaN-safe: NaN fails both comparisons and becomes 0).
        if (!(loadPercent > 0.0)) loadPercent = 0.0;
        if (loadPercent > 100.0)  loadPercent = 100.0;

        double watts = idle + loadPercent / 100.0 * (tdp - idle);
        if (watts > tdp)
            watts = tdp;
        return watts;
    }
}
