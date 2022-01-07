// Copyright 2021 NXP
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package imx_external_camera

import (
        "android/soong/android"
        "android/soong/cc"
        "strings"
)

func init() {
    android.RegisterModuleType("imx_external_camera_defaults", imx_CameraDefaultsFactory)
}
func imx_CameraDefaultsFactory() (android.Module) {
    module := cc.DefaultsFactory()
    android.AddLoadHook(module, imx_CameraDefaults)
    return module
}

func imx_CameraDefaults(ctx android.LoadHookContext) {
    type props struct {
        Target struct {
                Android struct {
                        Shared_libs []string
                        Header_libs []string
                }
        }
    }
    p := &props{}
    var board string = ctx.Config().VendorConfig("IMXPLUGIN").String("BOARD_PLATFORM")
    if strings.Contains(board, "imx") {
        p.Target.Android.Shared_libs = append(p.Target.Android.Shared_libs, "camera.device@3.4-external-imx-impl")
        p.Target.Android.Header_libs = append(p.Target.Android.Header_libs, "camera.device@3.4-external-imx-impl_headers")
    } else {
        p.Target.Android.Shared_libs = append(p.Target.Android.Shared_libs, "camera.device@3.4-external-impl")
        p.Target.Android.Header_libs = append(p.Target.Android.Header_libs, "camera.device@3.4-external-impl_headers")
    }
    ctx.AppendProperties(p)
}