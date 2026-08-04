# Third-party notices

This file records sources that informed parts of the firmware and the licenses
for the direct PlatformIO libraries pinned in `platformio.ini`.

## pyacaia

The Acaia protocol implementation in `src/ble/` was informed by
[pyacaia](https://github.com/lucapinello/pyacaia), including packet constants,
event mappings, and legacy scale discovery behavior.

pyacaia is copyright 2019 Luca Pinello and is distributed under the GNU Affero
General Public License version 3. Pump Bug firmware is distributed under the
same license. See `LICENSE` for the license terms.

## M5StickS3 board support

`boards/m5stick-s3.json` and
`variants/m5stack_sticks3/pins_arduino.h` are adapted from the M5Stack Arduino
ESP32 board package version 3.3.7. That package is based on
[Espressif's Arduino core for ESP32](https://github.com/espressif/arduino-esp32)
and is distributed under the GNU Lesser General Public License version 2.1 or
later.

## FFT implementation

`src/util/fft.h` and `src/util/fft.cpp` are adapted from the `Mic_FFT` example
in [M5Unified](https://github.com/m5stack/M5Unified), copyright 2021 M5Stack
and distributed under the MIT License. Pump Bug's modifications are copyright
2026 ludable and distributed under the GNU Affero General Public License
version 3. The applicable notice appears under
[M5Unified and M5GFX license](#m5unified-and-m5gfx-license).

## BMI270 wake-on-motion implementation

The direct-register BMI270 wake-on-motion implementation in
`src/util/power.cpp` was informed by these sources:

- [M5StickS3 wake-on-motion documentation](https://docs.m5stack.com/en/arduino/m5sticks3/m5pm1),
  which established the wake-on-motion approach;
- [M5Unified](https://github.com/m5stack/M5Unified), including its BMI270
  register definitions and initialization behavior;
- [SparkFun BMI270 Arduino Library](https://github.com/sparkfun/SparkFun_BMI270_Arduino_Library),
  which configured wake-on-motion but could not be integrated safely because
  it restarted the shared I2C bus; the library also provided the bundled Bosch
  SensorAPI source used during the register-level investigation; and
- [Bosch BMI270 SensorAPI](https://github.com/boschsensortec/BMI270_SensorAPI),
  including its feature table, any-motion enable logic, and interrupt
  definitions.

### Bosch BMI270 SensorAPI license

Copyright (c) 2023 Bosch Sensortec GmbH. All rights reserved.

BSD-3-Clause

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
3. Neither the name of the copyright holder nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

### SparkFun BMI270 Arduino Library license

The MIT License (MIT)

Copyright (c) 2020 SparkFun Electronics

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## Direct firmware libraries

The firmware directly links M5Unified 0.2.14, M5GFX 0.2.20, M5PM1 1.0.6, and
NimBLE-Arduino 2.5.0. The versions are pinned in `platformio.ini`.

### M5Unified and M5GFX license

M5Unified and M5GFX carry the same M5Stack MIT notice:

MIT License

Copyright (c) 2021 M5Stack

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

### M5PM1 license

MIT License

Copyright (c) 2025 M5Stack Technology CO LTD

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

### NimBLE-Arduino license

Apache License
Version 2.0, January 2004
http://www.apache.org/licenses/

TERMS AND CONDITIONS FOR USE, REPRODUCTION, AND DISTRIBUTION

1. Definitions.

   "License" shall mean the terms and conditions for use, reproduction, and
   distribution as defined by Sections 1 through 9 of this document.

   "Licensor" shall mean the copyright owner or entity authorized by the
   copyright owner that is granting the License.

   "Legal Entity" shall mean the union of the acting entity and all other
   entities that control, are controlled by, or are under common control with
   that entity. For the purposes of this definition, "control" means (i) the
   power, direct or indirect, to cause the direction or management of such
   entity, whether by contract or otherwise, or (ii) ownership of fifty percent
   (50%) or more of the outstanding shares, or (iii) beneficial ownership of
   such entity.

   "You" (or "Your") shall mean an individual or Legal Entity exercising
   permissions granted by this License.

   "Source" form shall mean the preferred form for making modifications,
   including but not limited to software source code, documentation source, and
   configuration files.

   "Object" form shall mean any form resulting from mechanical transformation
   or translation of a Source form, including but not limited to compiled object
   code, generated documentation, and conversions to other media types.

   "Work" shall mean the work of authorship, whether in Source or Object form,
   made available under the License, as indicated by a copyright notice that is
   included in or attached to the work (an example is provided in the Appendix
   below).

   "Derivative Works" shall mean any work, whether in Source or Object form,
   that is based on (or derived from) the Work and for which the editorial
   revisions, annotations, elaborations, or other modifications represent, as a
   whole, an original work of authorship. For the purposes of this License,
   Derivative Works shall not include works that remain separable from, or
   merely link (or bind by name) to the interfaces of, the Work and Derivative
   Works thereof.

   "Contribution" shall mean any work of authorship, including the original
   version of the Work and any modifications or additions to that Work or
   Derivative Works thereof, that is intentionally submitted to Licensor for
   inclusion in the Work by the copyright owner or by an individual or Legal
   Entity authorized to submit on behalf of the copyright owner. For the
   purposes of this definition, "submitted" means any form of electronic,
   verbal, or written communication sent to the Licensor or its representatives,
   including but not limited to communication on electronic mailing lists,
   source code control systems, and issue tracking systems that are managed by,
   or on behalf of, the Licensor for the purpose of discussing and improving the
   Work, but excluding communication that is conspicuously marked or otherwise
   designated in writing by the copyright owner as "Not a Contribution."

   "Contributor" shall mean Licensor and any individual or Legal Entity on
   behalf of whom a Contribution has been received by Licensor and subsequently
   incorporated within the Work.

2. Grant of Copyright License. Subject to the terms and conditions of this
   License, each Contributor hereby grants to You a perpetual, worldwide,
   non-exclusive, no-charge, royalty-free, irrevocable copyright license to
   reproduce, prepare Derivative Works of, publicly display, publicly perform,
   sublicense, and distribute the Work and such Derivative Works in Source or
   Object form.

3. Grant of Patent License. Subject to the terms and conditions of this License,
   each Contributor hereby grants to You a perpetual, worldwide, non-exclusive,
   no-charge, royalty-free, irrevocable (except as stated in this section) patent
   license to make, have made, use, offer to sell, sell, import, and otherwise
   transfer the Work, where such license applies only to those patent claims
   licensable by such Contributor that are necessarily infringed by their
   Contribution(s) alone or by combination of their Contribution(s) with the
   Work to which such Contribution(s) was submitted. If You institute patent
   litigation against any entity (including a cross-claim or counterclaim in a
   lawsuit) alleging that the Work or a Contribution incorporated within the
   Work constitutes direct or contributory patent infringement, then any patent
   licenses granted to You under this License for that Work shall terminate as
   of the date such litigation is filed.

4. Redistribution. You may reproduce and distribute copies of the Work or
   Derivative Works thereof in any medium, with or without modifications, and in
   Source or Object form, provided that You meet the following conditions:

   (a) You must give any other recipients of the Work or Derivative Works a copy
       of this License; and

   (b) You must cause any modified files to carry prominent notices stating
       that You changed the files; and

   (c) You must retain, in the Source form of any Derivative Works that You
       distribute, all copyright, patent, trademark, and attribution notices
       from the Source form of the Work, excluding those notices that do not
       pertain to any part of the Derivative Works; and

   (d) If the Work includes a "NOTICE" text file as part of its distribution,
       then any Derivative Works that You distribute must include a readable
       copy of the attribution notices contained within such NOTICE file,
       excluding those notices that do not pertain to any part of the Derivative
       Works, in at least one of the following places: within a NOTICE text file
       distributed as part of the Derivative Works; within the Source form or
       documentation, if provided along with the Derivative Works; or, within a
       display generated by the Derivative Works, if and wherever such
       third-party notices normally appear. The contents of the NOTICE file are
       for informational purposes only and do not modify the License. You may
       add Your own attribution notices within Derivative Works that You
       distribute, alongside or as an addendum to the NOTICE text from the Work,
       provided that such additional attribution notices cannot be construed as
       modifying the License.

   You may add Your own copyright statement to Your modifications and may
   provide additional or different license terms and conditions for use,
   reproduction, or distribution of Your modifications, or for any such
   Derivative Works as a whole, provided Your use, reproduction, and
   distribution of the Work otherwise complies with the conditions stated in
   this License.

5. Submission of Contributions. Unless You explicitly state otherwise, any
   Contribution intentionally submitted for inclusion in the Work by You to the
   Licensor shall be under the terms and conditions of this License, without any
   additional terms or conditions. Notwithstanding the above, nothing herein
   shall supersede or modify the terms of any separate license agreement you may
   have executed with Licensor regarding such Contributions.

6. Trademarks. This License does not grant permission to use the trade names,
   trademarks, service marks, or product names of the Licensor, except as
   required for reasonable and customary use in describing the origin of the
   Work and reproducing the content of the NOTICE file.

7. Disclaimer of Warranty. Unless required by applicable law or agreed to in
   writing, Licensor provides the Work (and each Contributor provides its
   Contributions) on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
   KIND, either express or implied, including, without limitation, any
   warranties or conditions of TITLE, NON-INFRINGEMENT, MERCHANTABILITY, or
   FITNESS FOR A PARTICULAR PURPOSE. You are solely responsible for determining
   the appropriateness of using or redistributing the Work and assume any risks
   associated with Your exercise of permissions under this License.

8. Limitation of Liability. In no event and under no legal theory, whether in
   tort (including negligence), contract, or otherwise, unless required by
   applicable law (such as deliberate and grossly negligent acts) or agreed to
   in writing, shall any Contributor be liable to You for damages, including any
   direct, indirect, special, incidental, or consequential damages of any
   character arising as a result of this License or out of the use or inability
   to use the Work (including but not limited to damages for loss of goodwill,
   work stoppage, computer failure or malfunction, or any and all other
   commercial damages or losses), even if such Contributor has been advised of
   the possibility of such damages.

9. Accepting Warranty or Additional Liability. While redistributing the Work or
   Derivative Works thereof, You may choose to offer, and charge a fee for,
   acceptance of support, warranty, indemnity, or other liability obligations
   and/or rights consistent with this License. However, in accepting such
   obligations, You may act only on Your own behalf and on Your sole
   responsibility, not on behalf of any other Contributor, and only if You agree
   to indemnify, defend, and hold each Contributor harmless for any liability
   incurred by, or claims asserted against, such Contributor by reason of your
   accepting any such warranty or additional liability.

END OF TERMS AND CONDITIONS

APPENDIX: How to apply the Apache License to your work.

To apply the Apache License to your work, attach the following boilerplate
notice, with the fields enclosed by brackets "{}" replaced with your own
identifying information. (Don't include the brackets!) The text should be
enclosed in the appropriate comment syntax for the file format. We also
recommend that a file or class name and description of purpose be included on
the same "printed page" as the copyright notice for easier identification
within third-party archives.

Copyright {yyyy} {name of copyright owner}

Licensed under the Apache License, Version 2.0 (the "License"); you may not use
this file except in compliance with the License. You may obtain a copy of the
License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software distributed
under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
CONDITIONS OF ANY KIND, either express or implied. See the License for the
specific language governing permissions and limitations under the License.

### NimBLE-Arduino notice

esp-nimble-cpp
NimBLE-Arduino
Copyright 2020-2026 Ryan Powell <ryan@nable-embedded.io> and
esp-nimble-cpp, NimBLE-Arduino contributors.

The Initial Developer of some parts of this library, which are copied from,
derived from, or inspired by is, esp32-snippets, Copyright 2017 Neil Kolban.

If this library is used for commercial purposes, it is requested that the user
consider making a donation and/or sponsoring this project to support it's
ongoing development.
