package net.seilent.seifg

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

data class AppEntry(
    val label: String,
    val packageName: String,
    val icon: Drawable
)

data class AppConfig(
    var enabled: Boolean = false,
    var fps: Int = 0,
    var multiplier: Int = 2
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

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ConfigScreen() {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()

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
                                map[key] = AppConfig(
                                    enabled = true,
                                    fps = obj.optInt("fps", 0),
                                    multiplier = obj.optInt("multiplier", 2)
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
                title = { Text("seifg") },
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
                                        entry.put("fps", cfg.fps)
                                        entry.put("multiplier", cfg.multiplier)
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
                        val cfg = configs.getOrPut(app.packageName) { AppConfig() }
                        AppRow(app, cfg) { updated ->
                            configs = configs.toMutableMap().also { it[app.packageName] = updated }
                        }
                    }
                }
            }
        }
    }
}

@Composable
fun AppRow(app: AppEntry, config: AppConfig, onUpdate: (AppConfig) -> Unit) {
    val fpsOptions = listOf(0, 30, 45, 60)
    val multiplierOptions = listOf(2, 3)

    Column(modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.fillMaxWidth()) {
            Image(
                bitmap = app.icon.toBitmap(48, 48).asImageBitmap(),
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
            Row(
                modifier = Modifier.fillMaxWidth().padding(start = 52.dp, top = 4.dp, bottom = 4.dp),
                horizontalArrangement = Arrangement.spacedBy(12.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text("FPS", style = MaterialTheme.typography.labelMedium)
                SingleChoiceSegmentedButtonRow {
                    fpsOptions.forEachIndexed { index, fps ->
                        SegmentedButton(
                            selected = config.fps == fps,
                            onClick = { onUpdate(config.copy(fps = fps)) },
                            shape = SegmentedButtonDefaults.itemShape(index, fpsOptions.size)
                        ) {
                            Text(if (fps == 0) "Off" else "$fps")
                        }
                    }
                }

                Spacer(Modifier.width(8.dp))
                Text("×", style = MaterialTheme.typography.labelMedium)
                SingleChoiceSegmentedButtonRow {
                    multiplierOptions.forEachIndexed { index, mult ->
                        SegmentedButton(
                            selected = config.multiplier == mult,
                            onClick = { onUpdate(config.copy(multiplier = mult)) },
                            shape = SegmentedButtonDefaults.itemShape(index, multiplierOptions.size)
                        ) {
                            Text("${mult}x")
                        }
                    }
                }
            }
        }

        HorizontalDivider()
    }
}
