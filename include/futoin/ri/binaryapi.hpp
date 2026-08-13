//-----------------------------------------------------------------------------
// Copyright 2023-2026 FutoIn Project (https://futoin.org)
// Copyright 2023-2026 Andrey Galkin <andrey@futoin.org>
//
// Licensed under the FutoIn Public License 1.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://specs.futoin.org/LICENSE.txt
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//-----------------------------------------------------------------------------

#ifndef FUTOIN_RI_BINARYAPI_HPP
#define FUTOIN_RI_BINARYAPI_HPP
//---
#include <futoin/binarysteps.h>
#include <futoin/iasyncsteps.hpp>

#include <memory>
//---

namespace futoin {
    namespace ri {
        struct BinarySteps : FutoInAsyncSteps
        {
            explicit BinarySteps(IAsyncSteps* asi);
            explicit BinarySteps(IAsyncSteps& asi);
            ~BinarySteps();

            IAsyncSteps& asi;
            // likely no benefits from flags as aligned to 32-bit any way
            bool managed_;
            bool parallel_{false};
        };
        extern const ::FutoInAsyncStepsAPI binary_steps_api;
        extern const ::FutoInSyncAPI binary_sync_api;
        extern std::unique_ptr<IAsyncSteps> wrap_binary_steps(
                FutoInAsyncSteps&);
        extern void init_binary_sync(FutoInSync&);
    } // namespace ri
} // namespace futoin

//---
#endif // FUTOIN_RI_BINARYAPI_HPP
