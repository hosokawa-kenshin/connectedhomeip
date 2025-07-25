/*
 *
 *    Copyright (c) 2023 Project CHIP Authors
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

package matter.onboardingpayload

/** Enum values for possible bits in the onboarding paylod's discovery capabilities bitmask. */
@Suppress("MagicNumber")
enum class DiscoveryCapability(val bitIndex: Int) {
  SOFT_AP(0),
  BLE(1),
  ON_NETWORK(2),
  WIFI_PAF(3),
  NFC(4) /* Indicates if NFC-based Commissioning is supported */
}
