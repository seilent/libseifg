package net.seilent.seifg

import android.content.Context
import de.robv.android.xposed.IXposedHookLoadPackage
import de.robv.android.xposed.XC_MethodHook
import de.robv.android.xposed.XposedBridge
import de.robv.android.xposed.XposedHelpers
import de.robv.android.xposed.callbacks.XC_LoadPackage
import java.util.concurrent.atomic.AtomicBoolean

class XposedEntry : IXposedHookLoadPackage {

    companion object {
        private val loaded = AtomicBoolean(false)
    }

    override fun handleLoadPackage(lpparam: XC_LoadPackage.LoadPackageParam) {
        if (loaded.getAndSet(true)) return

        XposedHelpers.findAndHookMethod(
            "android.app.Application",
            lpparam.classLoader,
            "attach",
            Context::class.java,
            object : XC_MethodHook() {
                override fun afterHookedMethod(param: MethodHookParam) {
                    val context = param.args[0] as Context
                    try {
                        val dir = context.packageManager
                            .getApplicationInfo("net.seilent.seifg", 0)
                            .nativeLibraryDir
                        System.load("$dir/libshadowhook_nothing.so")
                        System.load("$dir/libshadowhook.so")
                        System.load("$dir/libseifg_android.so")
                        NativeBridge.install()
                        XposedBridge.log("seifg: loaded, install called")
                    } catch (e: Throwable) {
                        XposedBridge.log("seifg: failed")
                        XposedBridge.log(e)
                    }
                }
            }
        )
    }
}
