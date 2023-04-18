//-----------------------------------------------------------------------------
//   Copyright 2023 FutoIn Project
//   Copyright 2023 Andrey Galkin
//
//   Licensed under the Apache License, Version 2.0 (the "License");
//   you may not use this file except in compliance with the License.
//   You may obtain a copy of the License at
//
//       http://www.apache.org/licenses/LICENSE-2.0
//
//   Unless required by applicable law or agreed to in writing, software
//   distributed under the License is distributed on an "AS IS" BASIS,
//   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//   See the License for the specific language governing permissions and
//   limitations under the License.
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

            inline void before_call()
            {
                succeeded_ = false;
                waiting_ = false;
            }

            inline void after_call()
            {
                if (!last_error_.empty()) {
                    if (last_error_info_.empty()) {
                        throw futoin::Error(last_error_.c_str());
                    } else {
                        throw futoin::ExtError(
                                last_error_.c_str(), last_error_info_.c_str());
                    }
                }

                if (succeeded_) {
                    asi.success();
                } else {
                    waiting_ = true;
                }
            }
            IAsyncSteps& asi;
            futoin::string last_error_;
            futoin::string last_error_info_;
            // likely no benefits from flags as aligned to 32-bit any way
            bool managed_;
            bool parallel_{false};
            std::atomic<bool> succeeded_{false};
            std::atomic<bool> waiting_{false};
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
