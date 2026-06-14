package com.github.ulresh.homeaccounting.ui

import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.Image
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import com.github.ulresh.homeaccounting.model.CatalogEntry
import com.github.ulresh.homeaccounting.model.Event
import com.github.ulresh.homeaccounting.model.Store
import com.github.ulresh.homeaccounting.sync.PairInfo
import com.github.ulresh.homeaccounting.sync.Qr
import com.github.ulresh.homeaccounting.sync.SyncClient
import com.github.ulresh.homeaccounting.sync.SyncResult
import com.github.ulresh.homeaccounting.sync.SyncServer
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withContext
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

class MainActivity : ComponentActivity() {
    private lateinit var store: Store

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        store = Store(filesDir)
        store.load()
        store.ensureIdentity()
        setContent {
            MaterialTheme(colorScheme = lightColorScheme()) {
                AppRoot(store)
            }
        }
    }
}

private fun nowStr(): String = SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.US).format(Date())

private fun fmtCost(c: Double): String =
    if (c == c.toLong().toDouble()) c.toLong().toString() else "%.2f".format(c)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun AppRoot(store: Store) {
    var tick by remember { mutableIntStateOf(0) }
    var search by remember { mutableStateOf("") }
    var category by remember { mutableStateOf("") }   // "" = все

    var addOpen by remember { mutableStateOf(false) }
    var editing by remember { mutableStateOf<Event?>(null) }
    var syncOpen by remember { mutableStateOf(false) }
    var peopleOpen by remember { mutableStateOf(false) }
    var dbOpen by remember { mutableStateOf(false) }
    var menuOpen by remember { mutableStateOf(false) }
    var selectedKey by remember { mutableStateOf<String?>(null) }

    val events = remember(tick, search, category) {
        store.events().filter { e ->
            (search.isBlank() || e.subject.contains(search, ignoreCase = true)) &&
                (category.isBlank() || store.categoryOf(e.subject) == category)
        }
    }
    val categories = remember(tick) { listOf("") + store.catalog().map { it.category } }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("ДомУчёт") },
                actions = {
                    Box {
                        TextButton(onClick = { menuOpen = true }) { Text("Меню") }
                        DropdownMenu(expanded = menuOpen, onDismissRequest = { menuOpen = false }) {
                            DropdownMenuItem(text = { Text("Синхронизация") },
                                onClick = { menuOpen = false; syncOpen = true })
                            DropdownMenuItem(text = { Text("Люди") },
                                onClick = { menuOpen = false; peopleOpen = true })
                            DropdownMenuItem(text = { Text("База: ${store.database}") },
                                onClick = { menuOpen = false; dbOpen = true })
                        }
                    }
                }
            )
        },
        floatingActionButton = {
            FloatingActionButton(onClick = { addOpen = true }) { Text("+", style = MaterialTheme.typography.headlineMedium) }
        }
    ) { padding ->
        Column(Modifier.padding(padding).fillMaxSize()) {
            Row(Modifier.padding(8.dp).fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalAlignment = Alignment.CenterVertically) {
                OutlinedTextField(
                    value = search, onValueChange = { search = it },
                    label = { Text("Поиск") }, singleLine = true,
                    modifier = Modifier.weight(1f)
                )
                CategoryDropdown(categories, category) { category = it }
            }
            HorizontalDivider()
            if (events.isEmpty()) {
                Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    Text("Нет записей")
                }
            } else {
                LazyColumn(Modifier.fillMaxSize()) {
                    items(events, key = { it.key() }) { e ->
                        EventRow(
                            store, e,
                            selected = selectedKey == e.key(),
                            onLongPress = { selectedKey = e.key() },
                            onClick = { selectedKey = null },
                            onEdit = { editing = e; selectedKey = null },
                            onDelete = { store.deleteEvent(e); selectedKey = null; tick++ },
                        )
                        HorizontalDivider()
                    }
                }
            }
        }
    }

    if (addOpen) {
        EventDialog(store, null, onDismiss = { addOpen = false }) { dt, subj, cost, ppl, vol ->
            store.addEvent(dt, subj, cost, ppl, vol); addOpen = false; tick++
        }
    }
    editing?.let { ev ->
        EventDialog(store, ev, onDismiss = { editing = null }) { dt, subj, cost, ppl, vol ->
            store.editEvent(ev, dt, subj, cost, ppl, vol); editing = null; tick++
        }
    }
    if (peopleOpen) {
        PeopleDialog(store, onDismiss = { peopleOpen = false; tick++ })
    }
    if (dbOpen) {
        DatabaseDialog(store, onDismiss = { dbOpen = false }, onSwitched = { dbOpen = false; tick++ })
    }
    if (syncOpen) {
        SyncDialog(store, onDismiss = { syncOpen = false; store.load(); tick++ })
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun EventRow(
    store: Store,
    e: Event,
    selected: Boolean,
    onLongPress: () -> Unit,
    onClick: () -> Unit,
    onEdit: () -> Unit,
    onDelete: () -> Unit,
) {
    var confirmDel by remember { mutableStateOf(false) }
    ListItem(
        modifier = Modifier.combinedClickable(onClick = onClick, onLongClick = onLongPress),
        colors = ListItemDefaults.colors(
            containerColor = if (selected) MaterialTheme.colorScheme.surfaceVariant
            else MaterialTheme.colorScheme.surface
        ),
        headlineContent = { Text("${e.subject}   ${fmtCost(e.cost)}") },
        supportingContent = {
            val cat = store.categoryOf(e.subject)
            val extra = listOfNotNull(
                e.eventDatetime,
                if (cat.isNotEmpty()) cat else null,
                e.people,
                e.volume,
            ).joinToString("  ·  ")
            Text(extra)
        },
        trailingContent = if (selected) {
            {
                Row {
                    TextButton(onClick = onEdit) { Text("Изм.") }
                    TextButton(onClick = { confirmDel = true }) { Text("Удал.") }
                }
            }
        } else null
    )
    if (confirmDel) {
        AlertDialog(
            onDismissRequest = { confirmDel = false },
            title = { Text("Удаление") },
            text = { Text("Удалить «${e.subject}»?") },
            confirmButton = { TextButton(onClick = { confirmDel = false; onDelete() }) { Text("Да") } },
            dismissButton = { TextButton(onClick = { confirmDel = false }) { Text("Нет") } }
        )
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun CategoryDropdown(options: List<String>, selected: String, onSelect: (String) -> Unit) {
    var expanded by remember { mutableStateOf(false) }
    val label = if (selected.isBlank()) "Все" else selected
    Box {
        OutlinedButton(onClick = { expanded = true }) { Text(label) }
        DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
            options.forEach { opt ->
                DropdownMenuItem(
                    text = { Text(if (opt.isBlank()) "Все" else opt) },
                    onClick = { onSelect(opt); expanded = false }
                )
            }
        }
    }
}

@Composable
private fun EventDialog(
    store: Store,
    edit: Event?,
    onDismiss: () -> Unit,
    onSave: (dt: String, subject: String, cost: Double, people: String?, volume: String?) -> Unit,
) {
    var subject by remember { mutableStateOf(edit?.subject ?: "") }
    var cost by remember { mutableStateOf(edit?.cost?.let { fmtCost(it) } ?: "") }
    var dt by remember { mutableStateOf(edit?.eventDatetime ?: nowStr()) }
    var people by remember { mutableStateOf(edit?.people ?: "") }
    var volume by remember { mutableStateOf(edit?.volume ?: "") }
    var subjMenu by remember { mutableStateOf(false) }

    val items = remember { store.catalog().flatMap { it.items }.distinct() }
    val peopleList = remember { store.people() }
    var pplMenu by remember { mutableStateOf(false) }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(if (edit == null) "Новая запись" else "Изменить запись") },
        text = {
            Column(
                Modifier.verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                Box {
                    OutlinedTextField(
                        value = subject, onValueChange = { subject = it },
                        label = { Text("Наименование") }, singleLine = true,
                        trailingIcon = { TextButton(onClick = { subjMenu = true }) { Text("▾") } },
                        modifier = Modifier.fillMaxWidth()
                    )
                    DropdownMenu(expanded = subjMenu, onDismissRequest = { subjMenu = false }) {
                        items.forEach { it0 ->
                            DropdownMenuItem(text = { Text(it0) },
                                onClick = { subject = it0; subjMenu = false })
                        }
                    }
                }
                OutlinedTextField(
                    value = cost, onValueChange = { cost = it.replace(',', '.') },
                    label = { Text("Стоимость") }, singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                    modifier = Modifier.fillMaxWidth()
                )
                OutlinedTextField(
                    value = dt, onValueChange = { dt = it },
                    label = { Text("Дата (yyyy-MM-dd HH:mm)") }, singleLine = true,
                    modifier = Modifier.fillMaxWidth()
                )
                Box {
                    OutlinedTextField(
                        value = people, onValueChange = { people = it },
                        label = { Text("Кому") }, singleLine = true,
                        trailingIcon = { TextButton(onClick = { pplMenu = true }) { Text("▾") } },
                        modifier = Modifier.fillMaxWidth()
                    )
                    DropdownMenu(expanded = pplMenu, onDismissRequest = { pplMenu = false }) {
                        peopleList.forEach { p ->
                            DropdownMenuItem(text = { Text(p) },
                                onClick = { people = p; pplMenu = false })
                        }
                    }
                }
                OutlinedTextField(
                    value = volume, onValueChange = { volume = it },
                    label = { Text("Количество") }, singleLine = true,
                    modifier = Modifier.fillMaxWidth()
                )
            }
        },
        confirmButton = {
            TextButton(onClick = {
                if (subject.isNotBlank()) {
                    onSave(dt.trim(), subject.trim(), cost.toDoubleOrNull() ?: 0.0,
                        people.ifBlank { null }, volume.ifBlank { null })
                }
            }) { Text("Сохранить") }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Отмена") } }
    )
}

@Composable
private fun PeopleDialog(store: Store, onDismiss: () -> Unit) {
    var name by remember { mutableStateOf("") }
    var tick by remember { mutableIntStateOf(0) }
    val list = remember(tick) { store.people() }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Люди") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(if (list.isEmpty()) "Список пуст" else list.joinToString(", "))
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedTextField(value = name, onValueChange = { name = it },
                        label = { Text("Имя") }, singleLine = true, modifier = Modifier.weight(1f))
                    Button(onClick = {
                        if (name.isNotBlank()) { store.addPerson(name.trim()); name = ""; tick++ }
                    }) { Text("Добавить") }
                }
            }
        },
        confirmButton = { TextButton(onClick = onDismiss) { Text("Закрыть") } }
    )
}

@Composable
private fun DatabaseDialog(store: Store, onDismiss: () -> Unit, onSwitched: () -> Unit) {
    var name by remember { mutableStateOf(store.database) }
    val dbs = remember { store.databases() }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("База данных") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Текущая: ${store.database}")
                Text("Доступные: ${dbs.joinToString(", ")}")
                OutlinedTextField(value = name, onValueChange = { name = it },
                    label = { Text("Имя базы") }, singleLine = true)
            }
        },
        confirmButton = {
            TextButton(onClick = {
                if (name.isNotBlank()) { store.switchDatabase(name.trim(), true); onSwitched() }
            }) { Text("Переключить/Создать") }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Отмена") } }
    )
}

@Composable
private fun SyncDialog(store: Store, onDismiss: () -> Unit) {
    val scope = rememberCoroutineScope()
    val mainHandler = remember { Handler(Looper.getMainLooper()) }

    var status by remember { mutableStateOf("") }
    var qrInfo by remember { mutableStateOf<PairInfo?>(null) }
    var busy by remember { mutableStateOf(false) }
    var ip by remember { mutableStateOf("") }
    var port by remember { mutableStateOf("") }
    var code by remember { mutableStateOf("") }

    var confirmReq by remember { mutableStateOf<Pair<String, CompletableDeferred<Boolean>>?>(null) }
    val confirmFn: (String) -> Boolean = { pubkey ->
        val def = CompletableDeferred<Boolean>()
        mainHandler.post { confirmReq = pubkey to def }
        runBlocking { def.await() }
    }

    fun showResult(r: SyncResult) {
        status = when {
            r.ok -> "✓ Передано ${r.sent}, принято ${r.received}"
            r.error == "db_mismatch" -> "Разные базы: у партнёра «${r.peerDb}». Переключитесь в меню «База»."
            r.error == "rejected" -> "Подключение отклонено"
            r.error == "bad_code" -> "Неверный код"
            else -> "Ошибка: ${r.error}"
        }
        busy = false
    }

    val server = remember { SyncServer(store) }
    val scanLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { res ->
        val text = res.data?.getStringExtra(ScannerActivity.EXTRA_RESULT)
        if (text != null) {
            val info = PairInfo.fromJson(text)
            busy = true; status = "Подключение…"
            scope.launch {
                val r = withContext(Dispatchers.IO) { SyncClient(store).connect(info, confirmFn) }
                showResult(r)
            }
        }
    }
    val context = androidx.compose.ui.platform.LocalContext.current

    AlertDialog(
        onDismissRequest = { server.cancel(); onDismiss() },
        title = { Text("Синхронизация") },
        text = {
            Column(
                Modifier.verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(10.dp)
            ) {
                // Сервер
                Text("Принять подключение (показать QR):", style = MaterialTheme.typography.titleSmall)
                Button(enabled = !busy, onClick = {
                    busy = true; status = "Ожидание подключения…"
                    val info = server.listen()
                    qrInfo = info
                    scope.launch {
                        val r = withContext(Dispatchers.IO) { server.waitAndSync(confirmFn) }
                        showResult(r)
                    }
                }) { Text("Старт") }
                qrInfo?.let { info ->
                    val bmp = remember(info) { Qr.encode(info.toJson(), 500) }
                    Image(bitmap = bmp.asImageBitmap(), contentDescription = "QR",
                        modifier = Modifier.size(220.dp))
                    Text("IP: ${info.ip}  Порт: ${info.port}\nКод: ${info.code}\nБаза: ${info.db}")
                }

                HorizontalDivider()

                // Клиент
                Text("Подключиться:", style = MaterialTheme.typography.titleSmall)
                Button(enabled = !busy, onClick = {
                    scanLauncher.launch(Intent(context, ScannerActivity::class.java))
                }) { Text("Сканировать QR") }
                Text("или вручную:")
                OutlinedTextField(value = ip, onValueChange = { ip = it },
                    label = { Text("IP") }, singleLine = true, modifier = Modifier.fillMaxWidth())
                OutlinedTextField(value = port, onValueChange = { port = it },
                    label = { Text("Порт") }, singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                    modifier = Modifier.fillMaxWidth())
                OutlinedTextField(value = code, onValueChange = { code = it },
                    label = { Text("Код") }, singleLine = true, modifier = Modifier.fillMaxWidth())
                Button(enabled = !busy, onClick = {
                    val info = PairInfo(ip.trim(), port.trim().toIntOrNull() ?: 0, code.trim(), store.database)
                    if (info.ip.isNotEmpty() && info.port != 0 && info.code.isNotEmpty()) {
                        busy = true; status = "Подключение…"
                        scope.launch {
                            val r = withContext(Dispatchers.IO) { SyncClient(store).connect(info, confirmFn) }
                            showResult(r)
                        }
                    }
                }) { Text("Синхронизировать") }

                if (status.isNotEmpty()) Text(status)
            }
        },
        confirmButton = { TextButton(onClick = { server.cancel(); onDismiss() }) { Text("Закрыть") } }
    )

    confirmReq?.let { (pubkey, def) ->
        AlertDialog(
            onDismissRequest = { },
            title = { Text("Новое устройство") },
            text = { Text("Разрешить синхронизацию?\n${pubkey.take(48)}…") },
            confirmButton = { TextButton(onClick = { def.complete(true); confirmReq = null }) { Text("Да") } },
            dismissButton = { TextButton(onClick = { def.complete(false); confirmReq = null }) { Text("Нет") } }
        )
    }
}
