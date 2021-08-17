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

=============================
The Representor Plug Provider
=============================

Logical Switch Port Options
---------------------------

plug:representor:pf-mac
~~~~~~~~~~~~~~~~~~~~~~~

MAC address for identifying PF device.  When
`OVN_Northbound:Logical_Switch_Port:options` key `plug:representor:vf-num` is
also set, this option is used to identify PF to use as base to locate the
correct VF representor port.  When `OVN_Northbound:Logical_Switch_Port:options`
key `plug:representor:vf-num` is not set this option is used to locate a PF
representor port.

plug:representor:vf-num
~~~~~~~~~~~~~~~~~~~~~~~

Logical VF number relative to PF device specified in
`OVN_Northbound:Logical_Switch_Port:options` key `plug-pf-mac`.
