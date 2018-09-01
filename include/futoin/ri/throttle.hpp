//-----------------------------------------------------------------------------
//   Copyright 2018 FutoIn Project
//   Copyright 2018 Andrey Galkin
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

#ifndef FUTOIN_RI_THROTTLE_HPP
#define FUTOIN_RI_THROTTLE_HPP
//---
#include <futoin/iasyncsteps.hpp>
//---
#include <mutex>

namespace futoin {
    namespace ri {
        template<typename OSMutex>
        class BaseThrottle : public ISync
        {
        public:
            void lock(IAsyncSteps& /*asi*/) override {}
            void unlock(IAsyncSteps& /*asi*/) noexcept override {}
        };

        using ThreadlessThrottle = BaseThrottle<ISync::NoopOSMutex>;
        using Throttle = BaseThrottle<std::mutex>;
    } // namespace ri
} // namespace futoin

//---
#endif // FUTOIN_RI_THROTTLE_HPP
