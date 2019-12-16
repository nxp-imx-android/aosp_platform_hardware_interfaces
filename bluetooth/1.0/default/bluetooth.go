package bluetooth

import (
        "android/soong/android"
        "android/soong/cc"
)

func init() {
    android.RegisterModuleType("bluetooth_defaults", bluetoothDefaultsFactory)
}

func bluetoothDefaultsFactory() (android.Module) {
    module := cc.DefaultsFactory()
    android.AddLoadHook(module, bluetoothDefaults)
    return module
}

func bluetoothDefaults(ctx android.LoadHookContext) {
    var cppflags []string
    type props struct {
        Cflags []string
    }

    p := &props{}
    if ctx.Config().VendorConfig("IMXPLUGIN").Bool("BOARD_BLUETOOTH_NO_DLE") {
        cppflags = append(cppflags, "-DBOARD_BLUETOOTH_NO_DLE")
    }
    p.Cflags = cppflags
    ctx.AppendProperties(p)
}
