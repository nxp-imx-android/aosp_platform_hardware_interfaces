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
    if ctx.Config().VendorConfig("IMXPLUGIN").Bool("BOARD_HAVE_BLUETOOTH_QCOM") {
        cppflags = append(cppflags, "-DBOARD_HAVE_BLUETOOTH_QCOM")
    }
    p.Cflags = cppflags
    ctx.AppendProperties(p)
}
