// User-implementable handler bodies for OddPathMonitor.
//
// FIRST-TIME-ONLY SCAFFOLD. `artheia gen-app` checks for this file's
// existence and refuses to overwrite. If you delete it, the next
// regen will recreate the empty stubs.
//
// One method body per receiver port:
//   void OddPathMonitor::on_<port>(const GwMessageHeader& hdr,
//                                   const shared_<Pdu>& msg) noexcept
// The declarations are in OddPathMonitor.hh; only the bodies are yours.

#include "OddPathMonitor.hh"

#include <atomic>
#include <cstdio>

namespace odd_path_client {


void OddPathMonitor::on_acc_06(
    const GwMessageHeader& hdr,
    const mlbevo_gen2_ACC_06& msg) noexcept {
  (void)hdr; (void)msg;
  // TODO: handle acc_06 (ACC_06_Iface)
  // Decoded fields are nanopb POD; see flexray/ACC_06.pb.h for layout.
}

void OddPathMonitor::on_acc_07(
    const GwMessageHeader& hdr,
    const shared_ACC_07& msg) noexcept {
  // Log every 100th frame so the stream doesn't fire-hose the console.
  static std::atomic<uint64_t> count{0};
  uint64_t n = count.fetch_add(1) + 1;
  if (n == 1 || (n % 100) == 0) {
    std::fprintf(stderr,
        "[ACC_07 #%lu seq=%u]  CRC=%u BZ=%u "
        "Anhalteweg=%.2f Anhalten=%u Boost=%u Freilauf_Anf=%u "
        "Folgebeschl=%.3f STA_Verzoeg_Anf=%.3f Freigabe=%u\n",
        (unsigned long)n,
        (unsigned)hdr.tipc.sequence_num,
        (unsigned)msg.ACC_07_CRC,
        (unsigned)msg.ACC_07_BZ,
        (double)msg.ACC_Anhalteweg,
        (unsigned)msg.ACC_Anhalten,
        (unsigned)msg.ACC_Boost,
        (unsigned)msg.ACC_Freilauf_Anf,
        (double)msg.ACC_Folgebeschl,
        (double)msg.STA_Verzoeg_Anf,
        (unsigned)msg.STA_Verzoeg_Anf_Freigabe);
  }
}

void OddPathMonitor::on_acc_10(
    const GwMessageHeader& hdr,
    const mlbevo_gen2_ACC_10& msg) noexcept {
  (void)hdr; (void)msg;
  // TODO: handle acc_10 (ACC_10_Iface)
  // Decoded fields are nanopb POD; see flexray/ACC_10.pb.h for layout.
}

void OddPathMonitor::on_aeb_01(
    const GwMessageHeader& hdr,
    const mlbevo_gen2_AEB_01& msg) noexcept {
  (void)hdr; (void)msg;
  // TODO: handle aeb_01 (AEB_01_Iface)
  // Decoded fields are nanopb POD; see flexray/AEB_01.pb.h for layout.
}

void OddPathMonitor::on_eml_01(
    const GwMessageHeader& hdr,
    const mlbevo_gen2_EML_01& msg) noexcept {
  (void)hdr; (void)msg;
  // TODO: handle eml_01 (EML_01_Iface)
  // Decoded fields are nanopb POD; see flexray/EML_01.pb.h for layout.
}

void OddPathMonitor::on_epb_01(
    const GwMessageHeader& hdr,
    const shared_EPB_01& msg) noexcept {
  (void)hdr; (void)msg;
  // TODO: handle epb_01 (EPB_01_Iface)
  // Decoded fields are nanopb POD; see shared/EPB_01.pb.h for layout.
}

void OddPathMonitor::on_esp_05(
    const GwMessageHeader& hdr,
    const shared_ESP_05& msg) noexcept {
  (void)hdr; (void)msg;
  // TODO: handle esp_05 (ESP_05_Iface)
  // Decoded fields are nanopb POD; see shared/ESP_05.pb.h for layout.
}

void OddPathMonitor::on_esp_32(
    const GwMessageHeader& hdr,
    const shared_ESP_32& msg) noexcept {
  (void)hdr; (void)msg;
  // TODO: handle esp_32 (ESP_32_Iface)
  // Decoded fields are nanopb POD; see shared/ESP_32.pb.h for layout.
}

void OddPathMonitor::on_fas_vk_01(
    const GwMessageHeader& hdr,
    const mlbevo_gen2_FAS_VK_01& msg) noexcept {
  (void)hdr; (void)msg;
  // TODO: handle fas_vk_01 (FAS_VK_01_Iface)
  // Decoded fields are nanopb POD; see flexray/FAS_VK_01.pb.h for layout.
}

void OddPathMonitor::on_gra_acc_01(
    const GwMessageHeader& hdr,
    const shared_GRA_ACC_01& msg) noexcept {
  (void)hdr; (void)msg;
  // TODO: handle gra_acc_01 (GRA_ACC_01_Iface)
  // Decoded fields are nanopb POD; see shared/GRA_ACC_01.pb.h for layout.
}

void OddPathMonitor::on_getriebe_11(
    const GwMessageHeader& hdr,
    const shared_Getriebe_11& msg) noexcept {
  (void)hdr; (void)msg;
  // TODO: handle getriebe_11 (Getriebe_11_Iface)
  // Decoded fields are nanopb POD; see shared/Getriebe_11.pb.h for layout.
}

void OddPathMonitor::on_kas_01(
    const GwMessageHeader& hdr,
    const shared_KAS_01& msg) noexcept {
  (void)hdr; (void)msg;
  // TODO: handle kas_01 (KAS_01_Iface)
  // Decoded fields are nanopb POD; see shared/KAS_01.pb.h for layout.
}

void OddPathMonitor::on_ldw_02(
    const GwMessageHeader& hdr,
    const shared_LDW_02& msg) noexcept {
  (void)hdr; (void)msg;
  // TODO: handle ldw_02 (LDW_02_Iface)
  // Decoded fields are nanopb POD; see shared/LDW_02.pb.h for layout.
}

void OddPathMonitor::on_lwi_01(
    const GwMessageHeader& hdr,
    const shared_LWI_01& msg) noexcept {
  (void)hdr; (void)msg;
  // TODO: handle lwi_01 (LWI_01_Iface)
  // Decoded fields are nanopb POD; see shared/LWI_01.pb.h for layout.
}

void OddPathMonitor::on_licht_hinten_01(
    const GwMessageHeader& hdr,
    const shared_Licht_hinten_01& msg) noexcept {
  (void)hdr; (void)msg;
  // TODO: handle licht_hinten_01 (Licht_hinten_01_Iface)
  // Decoded fields are nanopb POD; see shared/Licht_hinten_01.pb.h for layout.
}

void OddPathMonitor::on_rcta_01(
    const GwMessageHeader& hdr,
    const shared_RCTA_01& msg) noexcept {
  (void)hdr; (void)msg;
  // TODO: handle rcta_01 (RCTA_01_Iface)
  // Decoded fields are nanopb POD; see shared/RCTA_01.pb.h for layout.
}

void OddPathMonitor::on_bv2_objektheader(
    const GwMessageHeader& hdr,
    const mlbevo_gen2_BV2_ObjektHeader& msg) noexcept {
  (void)hdr; (void)msg;
  // TODO: handle bv2_objektheader (BV2_ObjektHeader_Iface)
  // Decoded fields are nanopb POD; see flexray/BV2_ObjektHeader.pb.h for layout.
}

void OddPathMonitor::on_bv2_objekt_01(
    const GwMessageHeader& hdr,
    const mlbevo_gen2_BV2_Objekt_01& msg) noexcept {
  (void)hdr; (void)msg;
  // TODO: handle bv2_objekt_01 (BV2_Objekt_01_Iface)
  // Decoded fields are nanopb POD; see flexray/BV2_Objekt_01.pb.h for layout.
}

void OddPathMonitor::on_bv2_objekt_02(
    const GwMessageHeader& hdr,
    const mlbevo_gen2_BV2_Objekt_02& msg) noexcept {
  (void)hdr; (void)msg;
  // TODO: handle bv2_objekt_02 (BV2_Objekt_02_Iface)
  // Decoded fields are nanopb POD; see flexray/BV2_Objekt_02.pb.h for layout.
}

void OddPathMonitor::on_bv2_objekt_03(
    const GwMessageHeader& hdr,
    const mlbevo_gen2_BV2_Objekt_03& msg) noexcept {
  (void)hdr; (void)msg;
  // TODO: handle bv2_objekt_03 (BV2_Objekt_03_Iface)
  // Decoded fields are nanopb POD; see flexray/BV2_Objekt_03.pb.h for layout.
}

void OddPathMonitor::on_bv2_objekt_04(
    const GwMessageHeader& hdr,
    const mlbevo_gen2_BV2_Objekt_04& msg) noexcept {
  (void)hdr; (void)msg;
  // TODO: handle bv2_objekt_04 (BV2_Objekt_04_Iface)
  // Decoded fields are nanopb POD; see flexray/BV2_Objekt_04.pb.h for layout.
}

void OddPathMonitor::on_bv2_objekt_05(
    const GwMessageHeader& hdr,
    const mlbevo_gen2_BV2_Objekt_05& msg) noexcept {
  (void)hdr; (void)msg;
  // TODO: handle bv2_objekt_05 (BV2_Objekt_05_Iface)
  // Decoded fields are nanopb POD; see flexray/BV2_Objekt_05.pb.h for layout.
}

void OddPathMonitor::on_bv2_objekt_06(
    const GwMessageHeader& hdr,
    const mlbevo_gen2_BV2_Objekt_06& msg) noexcept {
  (void)hdr; (void)msg;
  // TODO: handle bv2_objekt_06 (BV2_Objekt_06_Iface)
  // Decoded fields are nanopb POD; see flexray/BV2_Objekt_06.pb.h for layout.
}

void OddPathMonitor::on_bv2_objekt_07(
    const GwMessageHeader& hdr,
    const mlbevo_gen2_BV2_Objekt_07& msg) noexcept {
  (void)hdr; (void)msg;
  // TODO: handle bv2_objekt_07 (BV2_Objekt_07_Iface)
  // Decoded fields are nanopb POD; see flexray/BV2_Objekt_07.pb.h for layout.
}

void OddPathMonitor::on_bv2_objekt_08(
    const GwMessageHeader& hdr,
    const mlbevo_gen2_BV2_Objekt_08& msg) noexcept {
  (void)hdr; (void)msg;
  // TODO: handle bv2_objekt_08 (BV2_Objekt_08_Iface)
  // Decoded fields are nanopb POD; see flexray/BV2_Objekt_08.pb.h for layout.
}

void OddPathMonitor::on_bv2_objekt_09(
    const GwMessageHeader& hdr,
    const mlbevo_gen2_BV2_Objekt_09& msg) noexcept {
  (void)hdr; (void)msg;
  // TODO: handle bv2_objekt_09 (BV2_Objekt_09_Iface)
  // Decoded fields are nanopb POD; see flexray/BV2_Objekt_09.pb.h for layout.
}

void OddPathMonitor::on_bv2_objekt_10(
    const GwMessageHeader& hdr,
    const mlbevo_gen2_BV2_Objekt_10& msg) noexcept {
  (void)hdr; (void)msg;
  // TODO: handle bv2_objekt_10 (BV2_Objekt_10_Iface)
  // Decoded fields are nanopb POD; see flexray/BV2_Objekt_10.pb.h for layout.
}

}  // namespace odd_path_client
