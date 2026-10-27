# Copyright (c) 2026 Dave, KY4WI
# SPDX-License-Identifier: GPL-3.0-or-later

"""Validate the firmware patch safety guards with synthetic images."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define main tkx4x_toolkit_original_main
#include "tkx4xtoolkit.c"
#undef main

static uint8_t image[PROGRAM_SIZE];
static uint8_t before[PROGRAM_SIZE];

static void seed(const struct tot_patch_region *r,size_t n,int32_t shift,
                 size_t patched)
{
    size_t i;
    for(i=0;i<n;i++){
        size_t at=(size_t)((int32_t)r[i].address+shift-(int32_t)PROGRAM_START);
        memcpy(image+at,i<patched?r[i].modified:r[i].original,r[i].length);
    }
}

#define CHECK(c) do {if(!(c)){fprintf(stderr,"Check failed at %d: %s\\n",__LINE__,#c);return 1;}}while(0)

int main(void)
{
    struct tot_patch_match m;
    struct app_state st;
    enum model_family model=MODEL_941;
    size_t i;

    /* Synthetic test images lack real DF00 firmware fingerprints.
       Public patching must reject them even with the exact TOT code. */
    memset(image,0xFF,sizeof(image));
    seed(ab00_tot_regions,3,0,0);
    memcpy(before,image,sizeof(image));
    CHECK(locate_tot_patch_for_model(image,model,&m)==TOT_UNSUPPORTED);
    CHECK(apply_tot_patch_for_model(image,model)<0);
    CHECK(!memcmp(image,before,sizeof(image)));

    /* The project signature must match exactly. Partial signatures and
       unrelated contents must not be overwritten. */
    CHECK(sizeof(AUTHOR_SIG_TEXT)-1u==AUTHOR_SIG_LEN);
    memset(image,0xFF,sizeof(image));
    CHECK(author_signature_writable(image+AUTHOR_SIG_OFFSET));
    memcpy(image+AUTHOR_SIG_OFFSET,AUTHOR_SIG_TEXT,AUTHOR_SIG_LEN);
    CHECK(author_signature_writable(image+AUTHOR_SIG_OFFSET));
    image[AUTHOR_SIG_OFFSET+AUTHOR_SIG_LEN-1u]=0xFF;
    CHECK(!author_signature_writable(image+AUTHOR_SIG_OFFSET));
    memcpy(image+AUTHOR_SIG_OFFSET,"KY4WI was here ",AUTHOR_SIG_LEN);
    CHECK(!author_signature_writable(image+AUTHOR_SIG_OFFSET));
    memcpy(image+AUTHOR_SIG_OFFSET,"KY4WI Fixed Tx ",AUTHOR_SIG_LEN);
    CHECK(!author_signature_writable(image+AUTHOR_SIG_OFFSET));
    CHECK(!memcmp(AUTHOR_SIG_TEXT,"KY4WI FW MODS  ",AUTHOR_SIG_LEN));
    memcpy(image,before,sizeof(image)); /* Restore the synthetic firmware. */

    /* Inject a recognized revision directly into the TEST-only helper.
       All known AB00 patch sites must match byte-for-byte. */
    CHECK(tot_classify_for_revision(image,&fw_db[2],model,&m)==TOT_STOCK);
    CHECK(m.count==3 && m.offsets[0]==(0x94E6u-PROGRAM_START));
    CHECK(apply_tot_patch_for_revision(image,&fw_db[2],model)==0);
    CHECK(tot_classify_for_revision(image,&fw_db[2],model,NULL)==TOT_PATCHED);
    CHECK(apply_tot_patch_for_revision(image,&fw_db[2],model)==0);

    /* Exact 10x TEST speed-up is recognized and safely reversible. */
    image[0x950Bu-PROGRAM_START]=0x01;
    image[0xB1A5u-PROGRAM_START]=0x01;
    CHECK(tot_test_10x_matches(image,&fw_db[2]));
    CHECK(tot_classify_for_revision(image,&fw_db[2],model,NULL)==TOT_PATCHED);
    CHECK(tot_test_10x_remove(image,&fw_db[2]));
    CHECK(!tot_test_10x_matches(image,&fw_db[2]));
    CHECK(tot_classify_for_revision(image,&fw_db[2],model,NULL)==TOT_PATCHED);
    image[0x950Bu-PROGRAM_START]=0x01;
    CHECK(!tot_test_10x_matches(image,&fw_db[2]));
    CHECK(tot_classify_for_revision(image,&fw_db[2],model,NULL)==TOT_UNSUPPORTED);
    image[0x950Bu-PROGRAM_START]=0x0A;

    /* Regression for the 9306 stock-image ETOT segfault. Fabricate
       a complete stock fingerprint from synthetic bytes: no Kenwood
       firmware is included. This must initialize the region offsets
       even when the full-image SHA classifies as exactly stock. */
    {
        struct firmware_revision rev9306=fw_db[FW_DB_COUNT-1u];
        const struct tot_patch_region *regions;
        struct app_state local_state;
        uint8_t restored[PROGRAM_SIZE];
        char synthetic_sha[65];
        size_t count=0;
        CHECK(rev9306.model==MODEL_840 && rev9306.checksum==0x9306);
        regions=tot_regions_for_revision(&rev9306,&count);
        CHECK(regions && count==2u);
        CHECK(regions[0].address==0x94EFu &&
              regions[1].address==0xB15Fu);
        memset(image,0xFF,PROGRAM_SIZE);
        seed(regions,count,0,0);
        memcpy(before,image,PROGRAM_SIZE);
        sha256_hex(image,PROGRAM_SIZE,synthetic_sha);
        rev9306.full_sha=synthetic_sha;

        memset(&m,0xA5,sizeof(m));
        CHECK(tk840_9306_tot_state(image,&rev9306)==TOT_STOCK);
        CHECK(tot_classify_for_revision(image,&rev9306,MODEL_840,&m)==TOT_STOCK);
        CHECK(m.regions==regions && m.count==2u);
        CHECK(m.offsets[0]==0x94EFu-PROGRAM_START);
        CHECK(m.offsets[1]==0xB15Fu-PROGRAM_START);

        CHECK(apply_tot_patch_for_revision(image,&rev9306,MODEL_840)==0);
        CHECK(tot_classify_for_revision(image,&rev9306,MODEL_840,NULL)==TOT_PATCHED);
        CHECK(apply_tot_patch_for_revision(image,&rev9306,MODEL_840)==0);

        memset(&local_state,0,sizeof(local_state));
        local_state.firmware_rev=&rev9306;
        memcpy(local_state.firmware,image,PROGRAM_SIZE);
        CHECK(restore_tk840_stock(&local_state,restored)==0);
        CHECK(!memcmp(restored,before,PROGRAM_SIZE));

        memcpy(image,before,PROGRAM_SIZE);
        image[regions[0].address-PROGRAM_START+6u]^=1u;
        memcpy(before,image,PROGRAM_SIZE);
        CHECK(apply_tot_patch_for_revision(image,&rev9306,MODEL_840)<0);
        CHECK(!memcmp(image,before,PROGRAM_SIZE));
    }

    /* Menu and confirmation distinguish enhanced TOT from the older bypass. */
    CHECK(!strcmp(tot_patch_action_label(TOT_PATCHED),
                  "Remove Enhanced TOT Patch"));
    CHECK(!strcmp(tot_patch_action_label(TOT_LEGACY),
                  "Remove Legacy TOT Bypass"));
    CHECK(!strcmp(tot_patch_action_label(TOT_STOCK),
                  "Apply Enhanced TOT Patch"));
    CHECK(!strcmp(tot_patch_action_label(TOT_UNSUPPORTED),
                  "Apply Enhanced TOT Patch"));

    /* TK-840 8355 has exactly two selected patch regions. */
    memset(image,0xFF,sizeof(image));
    seed(tk840_tot_regions,2,0,0);
    CHECK(tot_classify_for_revision(image,&fw_db[4],MODEL_840,&m)==TOT_STOCK);
    CHECK(m.count==2);
    CHECK(apply_tot_patch_for_revision(image,&fw_db[4],MODEL_840)==0);
    CHECK(tot_classify_for_revision(image,&fw_db[4],MODEL_840,NULL)==TOT_PATCHED);
    CHECK(tot_classify_for_revision(image,&fw_db[4],MODEL_941,NULL)==TOT_UNSUPPORTED);
    image[0x950Eu-PROGRAM_START]=0x01;
    image[0xB0E9u-PROGRAM_START]=0x01;
    CHECK(tot_test_10x_matches(image,&fw_db[4]));
    CHECK(tot_classify_for_revision(image,&fw_db[4],MODEL_840,NULL)==TOT_PATCHED);
    CHECK(tot_test_10x_remove(image,&fw_db[4]));
    CHECK(!tot_test_10x_matches(image,&fw_db[4]));
    CHECK(tot_classify_for_revision(image,&fw_db[4],MODEL_840,NULL)==TOT_PATCHED);

    /* Unknown source revisions must not use an otherwise identical recipe.
       B5EC is a known 940/941 revision, but has no TOT recipe. */
    memset(image,0xFF,sizeof(image));
    seed(ab00_tot_regions,3,0,0);
    memcpy(before,image,sizeof(image));
    CHECK(tot_classify_for_revision(image,NULL,MODEL_941,&m)==TOT_UNSUPPORTED);
    CHECK(tot_classify_for_revision(image,&fw_db[3],MODEL_941,&m)==TOT_UNSUPPORTED);
    CHECK(apply_tot_patch_for_revision(image,&fw_db[3],MODEL_941)<0);
    CHECK(!memcmp(image,before,sizeof(image)));

    /* Matching patch bytes at shifted addresses is not sufficient. */
    memset(image,0xFF,sizeof(image));
    seed(ab00_tot_regions,3,0x80,0);
    memcpy(before,image,sizeof(image));
    CHECK(tot_classify_for_revision(image,&fw_db[2],MODEL_941,&m)==TOT_UNSUPPORTED);
    CHECK(apply_tot_patch_for_revision(image,&fw_db[2],MODEL_941)<0);
    CHECK(!memcmp(image,before,sizeof(image)));

    /* Missing, mixed, or altered bytes refuse to patch and do not mutate. */
    memset(image,0xFF,sizeof(image));
    seed(ab00_tot_regions,2,0,0);
    memcpy(before,image,sizeof(image));
    CHECK(apply_tot_patch_for_revision(image,&fw_db[2],MODEL_941)<0);
    CHECK(!memcmp(image,before,sizeof(image)));

    memset(image,0xFF,sizeof(image));
    seed(ab00_tot_regions,3,0,1);
    memcpy(before,image,sizeof(image));
    CHECK(tot_classify_for_revision(image,&fw_db[2],MODEL_941,&m)==TOT_CONFLICT);
    CHECK(apply_tot_patch_for_revision(image,&fw_db[2],MODEL_941)<0);
    CHECK(!memcmp(image,before,sizeof(image)));

    memset(image,0xFF,sizeof(image));
    seed(ab00_tot_regions,3,0,0);
    image[0xB19Au+26u-PROGRAM_START]^=1u;
    CHECK(tot_classify_for_revision(image,&fw_db[2],MODEL_941,&m)==TOT_UNSUPPORTED);

    /* A historical AB00 unconditional TOT bypass is detected, not replaced. */
    memset(image,0xFF,sizeof(image));
    seed(ab00_tot_regions,3,0,0);
    image[0xB1AEu-PROGRAM_START]=0x00;
    image[0xB1AFu-PROGRAM_START]=0x00;
    memcpy(before,image,sizeof(image));
    CHECK(tot_classify_for_revision(image,&fw_db[2],MODEL_941,&m)==TOT_LEGACY);
    CHECK(apply_tot_patch_for_revision(image,&fw_db[2],MODEL_941)<0);
    CHECK(!memcmp(image,before,sizeof(image)));

    /* Patch selection cannot be overridden for unrecognized revisions. */
    memset(&st,0,sizeof(st));
    g_cli_noninteractive=1;
    st.model=MODEL_941;
    st.patch_selection_explicit=1;
    st.patch_selection=PATCH_SELECT_TOT;
    CHECK(choose_patch_selection(&st,NULL)==0);
    CHECK(choose_patch_selection(&st,&fw_db[3])==0);
    CHECK(choose_patch_selection(&st,&fw_db[2])==PATCH_SELECT_TOT);
    st.patch_selection=PATCH_SELECT_TX;
    CHECK(choose_patch_selection(&st,&fw_db[3])==PATCH_SELECT_TX);
    CHECK(choose_patch_selection(&st,&fw_db[4])==0);
    g_cli_noninteractive=0;

    /* No marker or code change is permitted in an unknown firmware image. */
    memset(image,0xFF,sizeof(image));
    seed(ab00_tot_regions,3,0,0);
    memcpy(before,image,sizeof(image));
    memset(&st,0,sizeof(st));
    st.patch_model=MODEL_941;
    CHECK(apply_selected_firmware_patches(image,&st,PATCH_SELECT_TOT)<0);
    CHECK(!memcmp(image,before,sizeof(image)));

    /* The known revision table remains explicit and finite. */
    CHECK(FW_FINGERPRINT_ADDR==0xDF00u);
    for(i=0;i<FW_DB_COUNT;i++)CHECK(fw_db[i].fingerprint_sha!=NULL);
    puts("Known-firmware-only patch regression tests passed.");
    return 0;
}
"""


class TestTOTPatchGuards(unittest.TestCase):
    @unittest.skipUnless(shutil.which("cc"), "C compiler not available")
    def test_exact_revision_patch_guards(self):
        with tempfile.TemporaryDirectory() as d:
            temp = Path(d)
            source = temp / "tot_patch_guard_test.c"
            binary = temp / "tot_patch_guard_test"
            source.write_text(SOURCE)
            compiled = subprocess.run(
                ["cc", "-std=c99", "-D_DEFAULT_SOURCE", "-D_POSIX_C_SOURCE=200809L",
                 "-O2", "-Wall", "-Wextra",
                 "-I", str(ROOT), str(source), str(ROOT / "fwtransmute.c"),
                 str(ROOT / "firmware_hex.c"), "-o", str(binary)],
                capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            executed = subprocess.run(
                [str(binary)], capture_output=True, text=True)
            self.assertEqual(
                executed.returncode, 0, executed.stdout + executed.stderr)


if __name__ == "__main__":
    unittest.main()
