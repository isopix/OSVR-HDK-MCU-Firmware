/*
 * Copyright 2016 OSVR and contributors.
 * Copyright 2016 Dennis Yeh.
 * Copyright 2016 Sensics, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#ifndef INCLUDED_EDID_Data_h_GUID_F2172D68_7DD2_46A7_C06B_38CF0AACB921
#define INCLUDED_EDID_Data_h_GUID_F2172D68_7DD2_46A7_C06B_38CF0AACB921

#include <stdint.h>

// info for #?f
const char svrEdidInfoString[] = "SVR1019, EDID spec v1.4, with 60Hz timings";

#if 0
// do not enforce the core key pass system - it's "accepted" by default
static uint8_t core_key_pass = 1;
// no matter what you enter if you choose to enter a key, it will be accepted as well.
#define CORE_KEY_PASS_FAIL_VALUE 1
#define CORE_KEY_PASS_SUCCESS_VALUE 1
#else
 // enforce the core key pass system
 static uint8_t core_key_pass = 0;

#define CORE_KEY_PASS_FAIL_VALUE 0
#define CORE_KEY_PASS_SUCCESS_VALUE 1
#endif

 /// SVR1019
 const unsigned char EDID_LUT[256] = {
     0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x4e, 0xd2, 0x19, 0x10, 0x00, 0x00, 0x00, 0x00, 0xff, 0x1a, 0x01,
     0x04, 0xa0, 0x49, 0x00, 0x78, 0x22, 0xa8, 0x7d, 0xab, 0x54, 0x3c, 0xb6, 0x21, 0x10, 0x4c, 0x4f, 0x00, 0x00, 0x00,
     0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x09, 0x4d, 0x70,
     0x28, 0x82, 0xb0, 0x0c, 0x40, 0xa4, 0x20, 0x63, 0x40, 0x04, 0x96, 0x10, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0xff,
     0x00, 0x53, 0x65, 0x6e, 0x73, 0x69, 0x63, 0x73, 0x20, 0x48, 0x44, 0x4b, 0x32, 0x0a, 0x00, 0x00, 0x00, 0xfc, 0x00,
     0x4f, 0x53, 0x56, 0x52, 0x20, 0x48, 0x44, 0x4b, 0x32, 0x0a, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x3a,
     0x5c, 0x0f, 0xb4, 0x1e, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x01, 0x73, 0x02, 0x03, 0x0a, 0x20, 0x65,
     0x03, 0x0c, 0x00, 0x10, 0x00, 0x09, 0x4d, 0x70, 0x28, 0x82, 0xb0, 0x0c, 0x40, 0xa4, 0x20, 0x63, 0x40, 0x04, 0x96,
     0x10, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb8};

#endif  // INCLUDED_EDID_Data_h_GUID_F2172D68_7DD2_46A7_C06B_38CF0AACB921