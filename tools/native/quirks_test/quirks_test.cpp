// Session 93 idea 6: offline table test for the SDK's driver quirks table (gpu_quirks.cpp). No game, no GPU.
// Feeds fake device properties for ~12 devices from docs\research\gpu_matrix_s93.md, prints header + decisions,
// and checks the expectations (exit code 1 on a failed one). Run: pwsh -File tools\native\quirks_test\run.ps1
#include <cstdio>
#include <cstring>
#include <string>

#include "gpu_quirks.h"

using namespace rex::ui::vulkan::quirks;

static int g_fail = 0;
#define EXPECT(cond, ...)                  \
  do {                                     \
    if (!(cond)) {                         \
      std::printf("  FAIL: " __VA_ARGS__); \
      std::printf("\n");                   \
      ++g_fail;                            \
    }                                      \
  } while (0)

static DeviceInfo Dev(uint32_t vendor, uint32_t type, uint32_t driver_id, uint32_t drv, const char* name,
                      const char* info = "", bool android = false) {
  DeviceInfo d;
  d.vendor_id = vendor;
  d.device_type = type;
  d.driver_id = driver_id;
  d.driver_version = drv;
  d.device_name = name;
  d.driver_info = info;
  d.is_android = android;
  d.max_storage_buffer_range = 128u << 20;
  d.max_per_stage_storage_buffers = 524288;
  d.max_dynamic_storage_buffers = 16;
  d.min_storage_buffer_offset_alignment = 64;
  d.max_push_constants = 256;
  return d;
}

static bool Has(const Result& r, const char* rule, const char* cvar = nullptr, const char* value = nullptr) {
  for (const Decision& d : r.decisions) {
    if (d.rule != rule) continue;
    if (cvar && d.cvar != cvar) continue;
    if (value && d.value != value) continue;
    return true;
  }
  return false;
}

static int SetCount(const Result& r, bool include_advisory) {
  int n = 0;
  for (const Decision& d : r.decisions)
    if (d.kind == Decision::kSetCvar && (include_advisory || !d.advisory)) ++n;
  return n;
}

static Result Show(const char* label, const DeviceInfo& d, const Options& o = Options()) {
  Result r = Evaluate(d, o);
  std::printf("== %s\n%s\n", label, HeaderLine(d, r).c_str());
  for (const Decision& x : r.decisions) {
    if (x.kind == Decision::kSetCvar)
      std::printf("[quirks] %s: %s=%s%s (%s)\n", x.rule.c_str(), x.cvar.c_str(), x.value.c_str(),
                  x.advisory ? " [advisory]" : "", x.reason.c_str());
    else
      std::printf("[quirks] %s: info (%s)\n", x.rule.c_str(), x.reason.c_str());
  }
  return r;
}

int main() {
  Result r;
  // Qualcomm: 0x80320037 = 512.800.55 (Jarrod's 830).
  r = Show("Adreno 830, 512.800.55",
           Dev(kVendorQualcomm, kTypeIntegrated, kDriverQualcommProprietary, 0x80320037, "Adreno (TM) 830", "", true));
  EXPECT(r.cls == GpuClass::kPhone && r.version.text == "512.800.55", "830 decode %s", r.version.text.c_str());
  EXPECT(Has(r, "qcom_custom_border_color", "vulkan_allow_custom_border_color", "false"), "830 border colour");
  for (const Decision& d : r.decisions)
    if (d.rule == "qcom_custom_border_color") EXPECT(!d.advisory, "border colour rule applies by default (Session 95 Lane D)");
  EXPECT(SetCount(r, false) == 2 && SetCount(r, true) == 3, "830: the eds list and the border rule applied by default (%d), 3 sets with advisory (%d)", SetCount(r, false), SetCount(r, true));
  EXPECT(!Has(r, "mali_dynamic_rendering_errata"), "830 no mali rule");
  EXPECT(!(r.eds_allowed & kEds1) && !(r.eds_allowed & kVertexInput) && (r.eds_allowed & kEds2), "830 eds %d",
         r.eds_allowed);

  // 512.676.3 = Adreno 720.
  r = Show("Adreno 720, 512.676.3 (EDS2 bug range)",
           Dev(kVendorQualcomm, kTypeIntegrated, kDriverQualcommProprietary, (512u << 22) | (676u << 12) | 3,
               "Adreno (TM) 720", "", true));
  EXPECT(r.version.text == "512.676.3", "720 decode %s", r.version.text.c_str());
  EXPECT(!(r.eds_allowed & kEds2), "720 eds2 off");

  // Mali-G57 r32p1.
  r = Show("Mali-G57, r32p1",
           Dev(kVendorArm, kTypeIntegrated, kDriverArmProprietary, (32u << 22) | (1u << 12), "Mali-G57 MC2",
               "v1.r32p1-00pre0.abcdef", true));
  EXPECT(r.cls == GpuClass::kPhone && r.version.text == "r32p1", "G57 %s", r.version.text.c_str());
  EXPECT(!Has(r, "mali_dynamic_rendering_errata"), "G57 not in the errata range");
  EXPECT(Has(r, "arm_robust_buffer_access"), "G57 robust note");

  // Mali-G715 r49p1: errata range.
  r = Show("Mali-G715, r49p1 (erratum 3786926)",
           Dev(kVendorArm, kTypeIntegrated, kDriverArmProprietary, (49u << 22) | (1u << 12), "Mali-G715",
               "v1.r49p1-01eac0.0123", true));
  EXPECT(Has(r, "mali_dynamic_rendering_errata", "vulkan_dynamic_rendering", "false"), "G715 r49p1 dyn rendering");

  // Hash-style driverVersion, driverInfo says r54p1 (Mali-G1): driverInfo wins; r54p1 is fixed.
  r = Show("Mali-G1-Ultra, hash driverVersion + r54p1 driverInfo",
           Dev(kVendorArm, kTypeIntegrated, kDriverArmProprietary, 0x08CBA525, "Mali-G1-Ultra MC12",
               "v1.r54p1-00eac0.xyz", true));
  EXPECT(r.version.text == "r54p1" && !Has(r, "mali_dynamic_rendering_errata"), "G1 %s", r.version.text.c_str());

  // Boundaries of the erratum: r49p0 no, r49p1 yes, r49p2 yes, r49p3 no, r50p0 yes, r51p0 yes, r51p1 no, r52p0 no.
  struct {
    int r, p;
    bool bad;
  } edges[] = {{49, 0, false}, {49, 1, true}, {49, 2, true}, {49, 3, false},
               {50, 0, true},  {51, 0, true}, {51, 1, false}, {52, 0, false}};
  for (auto& e : edges) {
    const std::string info = "v1.r" + std::to_string(e.r) + "p" + std::to_string(e.p) + "-x";
    Result x = Evaluate(Dev(kVendorArm, kTypeIntegrated, kDriverArmProprietary, 0, "Mali-G720", info.c_str(), true),
                        Options());
    EXPECT(Has(x, "mali_dynamic_rendering_errata") == e.bad, "edge r%dp%d expected %d", e.r, e.p, e.bad);
  }

  r = Show("Immortalis-G925, r49p1",
           Dev(kVendorArm, kTypeIntegrated, kDriverArmProprietary, (49u << 22) | (1u << 12), "Immortalis-G925 MC12",
               "v1.r49p1-01eac0.1", true));
  EXPECT(Has(r, "mali_dynamic_rendering_errata"), "G925 r49p1");

  // Xclipse 940 (Samsung driver 25.3.3).
  r = Show("Samsung Xclipse 940, 25.3.3",
           Dev(kVendorSamsung, kTypeIntegrated, kDriverSamsungProprietary, (25u << 22) | (3u << 12) | 3,
               "Samsung Xclipse 940", "", true));
  EXPECT(r.cls == GpuClass::kPhone && !(r.eds_allowed & kEds3ColorBlend), "Xclipse eds3 blend");

  // PowerVR BXM-8-256, changeset 6715691.
  r = Show("PowerVR BXM-8-256, 6715691",
           Dev(kVendorImagination, kTypeIntegrated, kDriverImaginationProprietary, 6715691, "PowerVR B-Series BXM-8-256",
               "25.1@6715691", true));
  EXPECT(r.cls == GpuClass::kPhone && r.version.text == "6715691", "BXM");

  // Intel UHD 620, Windows 101.2112 = (101 << 14) | 2112.
  r = Show("Intel UHD 620, 101.2112",
           Dev(kVendorIntel, kTypeIntegrated, kDriverIntelProprietaryWindows, (101u << 14) | 2112,
               "Intel(R) UHD Graphics 620"));
  EXPECT(r.cls == GpuClass::kIgpu && r.version.text == "101.2112" && !(r.eds_allowed & kVertexInput), "UHD");
  EXPECT(SetCount(r, true) == 1 && Has(r, "eds_block_list"), "UHD: only the eds list");

  r = Show("Intel Iris Xe, 101.5534",
           Dev(kVendorIntel, kTypeIntegrated, kDriverIntelProprietaryWindows, (101u << 14) | 5534,
               "Intel(R) Iris(R) Xe Graphics"));
  EXPECT(r.cls == GpuClass::kIgpu, "Iris Xe");

  // AMD Radeon 780M (Vulkan driver 2.0.416).
  r = Show("AMD Radeon 780M, 2.0.416",
           Dev(kVendorAmd, kTypeIntegrated, kDriverAmdProprietary, (2u << 22) | 416, "AMD Radeon(TM) 780M Graphics"));
  EXPECT(r.cls == GpuClass::kIgpu && !(r.eds_allowed & kEds3ColorBlend), "780M");

  // RTX 4090: no rule may fire. NVIDIA 581.15.0.0 is new enough for VIDS.
  r = Show("RTX 4090, 581.15",
           Dev(kVendorNvidia, kTypeDiscrete, kDriverNvidiaProprietary, (581u << 22) | (15u << 14),
               "NVIDIA GeForce RTX 4090"));
  EXPECT(r.cls == GpuClass::kDesktop && r.version.text == "581.15.0.0", "4090 %s", r.version.text.c_str());
  EXPECT(SetCount(r, true) == 0 && r.eds_allowed == kEdsAll, "4090: nothing may be set (%d)", SetCount(r, true));
  r = Evaluate(Dev(kVendorNvidia, kTypeDiscrete, kDriverNvidiaProprietary, (576u << 22) | (3u << 14),
                   "NVIDIA GeForce RTX 4090"),
               Options());
  EXPECT(SetCount(r, false) == 1 && Has(r, "eds_block_list") && !(r.eds_allowed & kVertexInput) &&
             (r.eds_allowed & kEds1),
         "4090 old driver: only the eds bits");
  {  // with the mobile switch on, a desktop is still untouched
    Options on;
    on.mobile_no_gs = true;
    r = Evaluate(Dev(kVendorNvidia, kTypeDiscrete, kDriverNvidiaProprietary, (581u << 22) | (15u << 14),
                     "NVIDIA GeForce RTX 4090"),
                 on);
    EXPECT(SetCount(r, true) == 0, "4090 with mobile_no_gs on");
  }

  // Turnip on an Adreno 740 (Mesa 25.1.5).
  r = Show("Turnip, Adreno 740, Mesa 25.1.5",
           Dev(kVendorQualcomm, kTypeIntegrated, kDriverMesaTurnip, (25u << 22) | (1u << 12) | 5,
               "Turnip Adreno (TM) 740", "", true));
  EXPECT(r.version.text == "25.1.5" && Has(r, "qcom_custom_border_color") && Has(r, "turnip_no_sparse", "vulkan_sparse_shared_memory", "false") && r.eds_allowed == kEdsAll, "turnip");

  // Software renderer.
  r = Show("llvmpipe", Dev(0x10005, kTypeCpu, kDriverMesaLlvmpipe, (25u << 22), "llvmpipe (LLVM 19.1.0, 256 bits)"));
  EXPECT(r.cls == GpuClass::kCpu, "llvmpipe");

  // The mobile switch: 830 with mobile_no_gs on.
  {
    Options on;
    on.mobile_no_gs = true;
    r = Show("Adreno 830 with gpu_quirks_mobile_no_gs=true",
             Dev(kVendorQualcomm, kTypeIntegrated, kDriverQualcommProprietary, 0x80320037, "Adreno (TM) 830", "", true),
             on);
    EXPECT(Has(r, "mobile_no_gs", "vulkan_force_expand_rectangle_lists_in_vs", "true") &&
               Has(r, "mobile_no_gs", "vulkan_force_expand_point_sprites_in_vs", "true") &&
               Has(r, "mobile_no_gs", "vulkan_force_convert_quad_lists_to_triangle_lists", "true"),
           "mobile_no_gs trio");
  }

  // vulkan_emulate_gpu parsing.
  {
    DeviceInfo base =
        Dev(kVendorNvidia, kTypeDiscrete, kDriverNvidiaProprietary, (581u << 22), "NVIDIA GeForce RTX 4090");
    const char* specs[] = {"5143:512.800.55:Adreno (TM) 830", "13B5:r49p1:Mali-G715:integrated",
                           "8086:101.2112:Intel(R) UHD Graphics 620", "10de:580.119.2.0:RTX Test:discrete",
                           "5143:0x80320037:Turnip Adreno 740"};
    for (const char* s : specs) {
      DeviceInfo e = base;
      std::string err;
      bool ok = ParseEmulateGpu(s, e, err);
      Result x = Evaluate(e, Options());
      std::printf("== emulate '%s' -> %s\n", s, ok ? HeaderLine(e, x).c_str() : err.c_str());
      EXPECT(ok, "emulate parse '%s': %s", s, err.c_str());
    }
    DeviceInfo e = base;
    std::string err;
    EXPECT(!ParseEmulateGpu("nonsense", e, err), "emulate nonsense must fail");
    EXPECT(!ParseEmulateGpu("zz:1.2.3:x", e, err), "emulate bad vendor must fail");
    DeviceInfo q = base;
    ParseEmulateGpu("5143:512.800.55:Adreno (TM) 830", q, err);
    EXPECT(q.driver_version == 0x80320037 && Evaluate(q, Options()).cls == GpuClass::kPhone, "emulate 830 == real 830");
  }

  // Session 95 (PLAIN): gpu_plain_mode. auto = plain only on a phone-class GPU of no known family or a virtual GPU on Android; PC classes never.
  {
    struct Case {
      const char* label;
      DeviceInfo dev;
      bool known, plain_auto;
      GpuClass cls;
    };
    const Case cases[] = {
        {"Adreno 830 stock", Dev(kVendorQualcomm, kTypeIntegrated, kDriverQualcommProprietary, 0x80320037, "Adreno (TM) 830", "", true), true, false, GpuClass::kPhone},
        {"Turnip 740", Dev(kVendorQualcomm, kTypeIntegrated, kDriverMesaTurnip, (25u << 22) | (1u << 12) | 5, "Turnip Adreno (TM) 740", "", true), true, false, GpuClass::kPhone},
        {"Mali-G57", Dev(kVendorArm, kTypeIntegrated, kDriverArmProprietary, (32u << 22) | (1u << 12), "Mali-G57 MC2", "v1.r32p1-00pre0.abcdef", true), true, false, GpuClass::kPhone},
        {"PowerVR BXM", Dev(kVendorImagination, kTypeIntegrated, kDriverImaginationProprietary, 6715691, "PowerVR B-Series BXM-8-256", "25.1@6715691", true), true, false, GpuClass::kPhone},
        {"Xclipse 940 (Samsung)", Dev(kVendorSamsung, kTypeIntegrated, kDriverSamsungProprietary, (25u << 22) | (3u << 12) | 3, "Samsung Xclipse 940", "", true), false, true, GpuClass::kPhone},
        {"unknown 19E5 integrated", Dev(0x19E5, kTypeIntegrated, kDriverUnknown, 1, "Maleoon 910", "", true), false, true, GpuClass::kPhone},
        {"unknown 19E5 discrete", Dev(0x19E5, kTypeDiscrete, kDriverUnknown, 1, "Maleoon 910", "", true), false, true, GpuClass::kPhone},
        {"unknown 19E5 other", Dev(0x19E5, kTypeOther, kDriverUnknown, 1, "Maleoon 910", "", true), false, true, GpuClass::kPhone},
        {"unknown 19E5 virtual (not cpu name)", Dev(0x19E5, kTypeVirtual, kDriverUnknown, 1, "Maleoon 910", "", true), false, true, GpuClass::kPhone},
        // (PLAIN review #6: a CPU renderer is out first; a virtual GPU on Android is plain, of any vendor; on a PC a virtual GPU stays as it was.)
        {"known vendor virtual on Android", Dev(kVendorQualcomm, kTypeVirtual, kDriverUnknown, 1, "Adreno (TM) 830", "", true), true, true, GpuClass::kPhone},
        {"venus virtual on Android (emulator, stays cpu class)", Dev(0x1AE0, kTypeVirtual, kDriverUnknown, 1, "Virtio-GPU Venus", "", true), false, true, GpuClass::kCpu},
        {"swiftshader reporting virtual on Android", Dev(0x1AE0, kTypeVirtual, kDriverGoogleSwiftshader, 1, "SwiftShader Device", "", true), false, false, GpuClass::kCpu},
        {"cpu type, unknown name on Android", Dev(0x19E5, kTypeCpu, kDriverUnknown, 1, "Maleoon 910", "", true), false, false, GpuClass::kCpu},
        {"virtual GPU on a PC", Dev(0x19E5, kTypeVirtual, kDriverUnknown, 1, "Maleoon 910"), false, false, GpuClass::kCpu},
        {"gfxstream on Android", Dev(0x1AE0, kTypeOther, kDriverUnknown, 1, "Android Emulator gfxstream", "", true), false, false, GpuClass::kCpu},
        {"swiftshader on Android", Dev(0x1AE0, kTypeCpu, kDriverGoogleSwiftshader, 1, "SwiftShader Device", "", true), false, false, GpuClass::kCpu},
        {"RTX 4090 (PC)", Dev(kVendorNvidia, kTypeDiscrete, kDriverNvidiaProprietary, (581u << 22) | (15u << 14), "NVIDIA GeForce RTX 4090"), false, false, GpuClass::kDesktop},
        {"UHD 620 (PC)", Dev(kVendorIntel, kTypeIntegrated, kDriverIntelProprietaryWindows, (101u << 14) | 2112, "Intel(R) UHD Graphics 620"), false, false, GpuClass::kIgpu},
        {"Radeon 780M (PC)", Dev(kVendorAmd, kTypeIntegrated, kDriverAmdProprietary, (2u << 22) | 416, "AMD Radeon(TM) 780M Graphics"), false, false, GpuClass::kIgpu},
        {"unknown vendor on a PC", Dev(0x19E5, kTypeIntegrated, kDriverUnknown, 1, "Maleoon 910"), false, false, GpuClass::kIgpu},
    };
    for (const Case& c : cases) {
      Options a;  // auto
      Result ra = Evaluate(c.dev, a);
      std::printf("== plain %s: class %s known %d plain(auto) %d (%s)\n", c.label, GpuClassName(ra.cls), IsKnownFamily(c.dev) ? 1 : 0,
                  ra.plain ? 1 : 0, ra.plain_why.c_str());
      EXPECT(ra.cls == c.cls, "plain %s class %s", c.label, GpuClassName(ra.cls));
      EXPECT(IsKnownFamily(c.dev) == c.known, "plain %s known", c.label);
      EXPECT(ra.plain == c.plain_auto, "plain %s auto expected %d", c.label, c.plain_auto ? 1 : 0);
      Options on;
      on.plain_mode = "on";
      EXPECT(Evaluate(c.dev, on).plain, "plain %s: on must force plain", c.label);
      Options off;
      off.plain_mode = "off";
      EXPECT(!Evaluate(c.dev, off).plain, "plain %s: off must never be plain", c.label);
      // the decisions (the flags every class sets) must not depend on the plain mode
      EXPECT(SetCount(Evaluate(c.dev, on), true) == SetCount(ra, true) && SetCount(Evaluate(c.dev, off), true) == SetCount(ra, true),
             "plain %s: the other quirk decisions changed with gpu_plain_mode", c.label);
    }
  }

  std::printf("\n%s (%d failed expectation%s)\n", g_fail ? "FAIL" : "PASS", g_fail, g_fail == 1 ? "" : "s");
  return g_fail ? 1 : 0;
}
