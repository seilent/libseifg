package com.seilent.sefg

import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ResolveInfo
import android.graphics.drawable.Drawable
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.core.graphics.drawable.toBitmap
import com.topjohnwu.superuser.Shell
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.File
import kotlin.math.abs

data class AppEntry(
    val label: String,
    val packageName: String,
    val icon: Drawable
)

fun validOutputs(refreshHz: Int, multiplier: Int): List<Int> {
    val results = mutableListOf<Int>()
    var k = 1
    while (refreshHz / k >= 30) {
        val output = refreshHz / k
        if (refreshHz % k == 0 && output % multiplier == 0 && output / multiplier >= 30) {
            results.add(output)
        }
        k++
    }
    return results
}

fun snapToNearest(current: Int, valid: List<Int>): Int {
    return valid.minByOrNull { abs(it - current) } ?: valid.firstOrNull() ?: 60
}

data class AppConfig(
    var enabled: Boolean = false,
    var targetFps: Int = 60,
    var multiplier: Int = 2,
    var quality: Int = 0
)

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            val colorScheme = if (Build.VERSION.SDK_INT >= 31) {
                dynamicDarkColorScheme(this)
            } else {
                darkColorScheme()
            }
            MaterialTheme(colorScheme = colorScheme) {
                Surface(modifier = Modifier.fillMaxSize()) {
                    ConfigScreen()
                }
            }
        }
    }
}

@Suppress("DEPRECATION")
fun getDisplayRefreshRate(activity: ComponentActivity): Int {
    val display = if (Build.VERSION.SDK_INT >= 30) {
        activity.display
    } else {
        val wm = activity.getSystemService(android.content.Context.WINDOW_SERVICE) as android.view.WindowManager
        wm.defaultDisplay
    }
    if (display != null) {
        val maxRate = display.supportedModes.maxOfOrNull { it.refreshRate }
        if (maxRate != null) return maxRate.toInt()
        return display.refreshRate.toInt()
    }
    return 60
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ConfigScreen() {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val refreshHz = remember { getDisplayRefreshRate(context as ComponentActivity) }

    var hasRoot by remember { mutableStateOf<Boolean?>(null) }
    var apps by remember { mutableStateOf<List<AppEntry>>(emptyList()) }
    var configs by remember { mutableStateOf(mutableMapOf<String, AppConfig>()) }
    var searchQuery by remember { mutableStateOf("") }
    var saving by remember { mutableStateOf(false) }
    var snackMessage by remember { mutableStateOf<String?>(null) }

    LaunchedEffect(Unit) {
        withContext(Dispatchers.IO) {
            Shell.getShell()
            hasRoot = Shell.isAppGrantedRoot() == true

            if (hasRoot == true) {
                val result = Shell.cmd("cat /data/local/tmp/TargetList.json").exec()
                if (result.isSuccess && result.out.isNotEmpty()) {
                    try {
                        val json = JSONObject(result.out.joinToString(""))
                        val custom = json.optJSONObject("custom")
                        if (custom != null) {
                            val map = mutableMapOf<String, AppConfig>()
                            for (key in custom.keys()) {
                                val obj = custom.getJSONObject(key)
                                val mult = obj.optInt("multiplier", 2).coerceIn(1, 3)
                                val validMults = listOf(1, 2, 3).filter { validOutputs(refreshHz, it).isNotEmpty() }
                                val effMult = if (mult in validMults) mult else (validMults.maxOrNull() ?: 1)
                                val fps = obj.optInt("fps", 30)
                                val targetFps = obj.optInt("target_fps", fps * effMult)
                                val quality = obj.optInt("quality", 0).coerceIn(0, 2)
                                val valid = validOutputs(refreshHz, effMult)
                                map[key] = AppConfig(
                                    enabled = true,
                                    targetFps = snapToNearest(targetFps, valid),
                                    multiplier = effMult,
                                    quality = quality
                                )
                            }
                            configs = map
                        }
                    } catch (_: Exception) {}
                }
            }

            val pm = context.packageManager
            val intent = Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
            val resolved: List<ResolveInfo> = pm.queryIntentActivities(intent, PackageManager.MATCH_ALL)
            apps = resolved
                .filter { it.activityInfo.packageName != context.packageName }
                .map { ri ->
                    AppEntry(
                        label = ri.loadLabel(pm).toString(),
                        packageName = ri.activityInfo.packageName,
                        icon = ri.loadIcon(pm)
                    )
                }
                .distinctBy { it.packageName }
                .sortedWith(compareByDescending<AppEntry> { configs.containsKey(it.packageName) }.thenBy { it.label.lowercase() })
        }
    }

    val filtered = remember(apps, searchQuery) {
        if (searchQuery.isBlank()) apps
        else apps.filter {
            it.label.contains(searchQuery, ignoreCase = true) ||
                    it.packageName.contains(searchQuery, ignoreCase = true)
        }
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("SeFG") },
                actions = {
                    TextButton(
                        onClick = {
                            saving = true
                            scope.launch(Dispatchers.IO) {
                                val json = JSONObject()
                                val custom = JSONObject()
                                for ((pkg, cfg) in configs) {
                                    if (cfg.enabled) {
                                        val entry = JSONObject()
                                        entry.put("fps", cfg.targetFps / cfg.multiplier)
                                        entry.put("multiplier", cfg.multiplier)
                                        entry.put("quality", cfg.quality)
                                        entry.put("target_fps", cfg.targetFps)
                                        custom.put(pkg, entry)
                                    }
                                }
                                json.put("custom", custom)

                                val cacheFile = File(context.cacheDir, "TargetList.json")
                                cacheFile.writeText(json.toString(2))

                                val result = Shell.cmd(
                                    "cp ${cacheFile.absolutePath} /data/local/tmp/TargetList.json && chmod 644 /data/local/tmp/TargetList.json"
                                ).exec()

                                withContext(Dispatchers.Main) {
                                    saving = false
                                    snackMessage = if (result.isSuccess) "Saved" else "Write failed"
                                }
                            }
                        },
                        enabled = hasRoot == true && !saving
                    ) {
                        Text("Save")
                    }
                }
            )
        },
        snackbarHost = {
            snackMessage?.let { msg ->
                Snackbar(
                    action = {
                        TextButton(onClick = { snackMessage = null }) { Text("OK") }
                    }
                ) { Text(msg) }
            }
        }
    ) { padding ->
        Column(modifier = Modifier.padding(padding)) {
            if (hasRoot == false) {
                Card(
                    colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.errorContainer),
                    modifier = Modifier.fillMaxWidth().padding(16.dp)
                ) {
                    Text(
                        "Root access unavailable. Cannot read or write config.",
                        modifier = Modifier.padding(16.dp),
                        color = MaterialTheme.colorScheme.onErrorContainer
                    )
                }
            }

            OutlinedTextField(
                value = searchQuery,
                onValueChange = { searchQuery = it },
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp),
                placeholder = { Text("Search apps") },
                singleLine = true
            )

            if (apps.isEmpty() && hasRoot != null) {
                Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    CircularProgressIndicator()
                }
            } else {
                LazyColumn(modifier = Modifier.fillMaxSize()) {
                    items(filtered, key = { it.packageName }) { app ->
                        val validMults = listOf(1, 2, 3).filter { validOutputs(refreshHz, it).isNotEmpty() }
                        val defaultMult = if (2 in validMults) 2 else (validMults.maxOrNull() ?: 1)
                        val defaultOutput = validOutputs(refreshHz, defaultMult).let { v ->
                            v.firstOrNull { it <= 60 } ?: v.firstOrNull() ?: 60
                        }
                        val cfg = configs.getOrPut(app.packageName) { AppConfig(targetFps = defaultOutput, multiplier = defaultMult) }
                        AppRow(app, cfg, refreshHz) { updated ->
                            configs = configs.toMutableMap().also { it[app.packageName] = updated }
                        }
                    }
                }
            }
        }
    }
}

@Composable
fun AppRow(app: AppEntry, config: AppConfig, refreshHz: Int, onUpdate: (AppConfig) -> Unit) {
    val multiplierOptions = listOf(1, 2, 3).filter { validOutputs(refreshHz, it).isNotEmpty() }
    val qualityLabels = listOf("Performance", "Balanced", "High")

    Column(modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.fillMaxWidth()) {
            Image(
                bitmap = app.icon.toBitmap(144, 144).asImageBitmap(),
                contentDescription = null,
                modifier = Modifier.size(40.dp)
            )
            Spacer(Modifier.width(12.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text(app.label, style = MaterialTheme.typography.bodyLarge)
                Text(app.packageName, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            Switch(
                checked = config.enabled,
                onCheckedChange = { onUpdate(config.copy(enabled = it)) }
            )
        }

        AnimatedVisibility(visible = config.enabled) {
            Column(modifier = Modifier.padding(start = 52.dp, top = 4.dp, bottom = 8.dp)) {
                Text("Multiplier", style = MaterialTheme.typography.labelMedium)
                SingleChoiceSegmentedButtonRow(modifier = Modifier.fillMaxWidth()) {
                    multiplierOptions.forEachIndexed { index, mult ->
                        SegmentedButton(
                            selected = config.multiplier == mult,
                            onClick = {
                                val newValid = validOutputs(refreshHz, mult)
                                val snapped = snapToNearest(config.targetFps, newValid)
                                onUpdate(config.copy(multiplier = mult, targetFps = snapped))
                            },
                            shape = SegmentedButtonDefaults.itemShape(index, multiplierOptions.size)
                        ) {
                            Text("${mult}x")
                        }
                    }
                }

                Spacer(Modifier.height(8.dp))

                val outputs = validOutputs(refreshHz, config.multiplier).sorted()

                Text("Output FPS", style = MaterialTheme.typography.labelMedium)
                SingleChoiceSegmentedButtonRow(modifier = Modifier.fillMaxWidth()) {
                    outputs.forEachIndexed { index, fps ->
                        SegmentedButton(
                            selected = config.targetFps == fps,
                            onClick = { onUpdate(config.copy(targetFps = fps)) },
                            shape = SegmentedButtonDefaults.itemShape(index, outputs.size)
                        ) {
                            Text("$fps")
                        }
                    }
                }

                Spacer(Modifier.height(8.dp))

                if (config.multiplier > 1) {
                    Text("Flow quality", style = MaterialTheme.typography.labelMedium)
                    SingleChoiceSegmentedButtonRow(modifier = Modifier.fillMaxWidth()) {
                        qualityLabels.forEachIndexed { index, label ->
                            SegmentedButton(
                                selected = config.quality == index,
                                onClick = { onUpdate(config.copy(quality = index)) },
                                shape = SegmentedButtonDefaults.itemShape(index, qualityLabels.size)
                            ) {
                                Text(label, style = MaterialTheme.typography.labelSmall)
                            }
                        }
                    }

                    Spacer(Modifier.height(8.dp))
                }

                val base = config.targetFps / config.multiplier
                Text(
                    "Output ${config.targetFps} FPS  |  render cap $base FPS",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        }

        HorizontalDivider()
    }
}
