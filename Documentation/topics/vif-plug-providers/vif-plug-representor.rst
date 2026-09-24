..
      Licensed under the Apache License, Version 2.0 (the "License"); you may
      not use this file except in compliance with the License. You may obtain
      a copy of the License at

          http://www.apache.org/licenses/LICENSE-2.0

      Unless required by applicable law or agreed to in writing, software
      distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
      WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
      License for the specific language governing permissions and limitations
      under the License.

      Convention for heading levels in OVN documentation:

      =======  Heading 0 (reserved for the title in a document)
      -------  Heading 1
      ~~~~~~~  Heading 2
      +++++++  Heading 3
      '''''''  Heading 4

      Avoid deeper levels because they do not render well.

=================================
The Representor VIF Plug Provider
=================================

Logical Switch Port Options
---------------------------

vif-plug:representor:pf-mac
~~~~~~~~~~~~~~~~~~~~~~~~~~~

MAC address for identifying PF device.  When
`OVN_Northbound:Logical_Switch_Port:options` key `vif-plug:representor:vf-num`
is also set, this option is used to identify PF to use as base to locate the
correct VF representor port.  When `OVN_Northbound:Logical_Switch_Port:options`
key `vif-plug:representor:vf-num` is not set this option is used to locate a PF
representor port.

vif-plug:representor:vf-num
~~~~~~~~~~~~~~~~~~~~~~~~~~~

Logical VF number relative to PF device specified in
`OVN_Northbound:Logical_Switch_Port:options` key `vif-plug-pf-mac`.

The VF MAC is programmed from the logical port MAC via devlink when running at
the DPU side. If VF MAC programming is not supported via devlink (for example
because of hardware, firmware, or driver limitations), ovn-vif logs this and
continues without failing the plug operation. In that case, the MAC address may
be programmed by hypervisor services (e.g. Libvirt), and it is up to the
operator to ensure an appropriate MAC programming method is in place. This
allows existing systems to keep working until kernel and/or firmware upgrades
add support, while platforms that do not support VF MAC programming continue to
use the graceful fallback.
