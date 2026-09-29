package dev.hermespebble.companion.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.unit.dp
import dev.hermespebble.companion.data.local.InkNoteEntity
import dev.hermespebble.companion.pebble.InkCodec

@Composable
fun HandwritingDetail(note: InkNoteEntity, onBack: () -> Unit) {
    var large by rememberSaveable(note.id) { mutableStateOf(false) }
    Column(Modifier.fillMaxSize()) {
        Text("Handwritten note", style = MaterialTheme.typography.headlineSmall)
        Text(java.text.DateFormat.getDateTimeInstance().format(java.util.Date(note.receivedAt)))
        Row {
            TextButton(onClick = onBack) { Text("All notes") }
            TextButton(onClick = { large = !large }) { Text(if (large) "Zoom out" else "Zoom in") }
        }
        Column(Modifier.weight(1f).verticalScroll(rememberScrollState())) {
            HandwritingCanvas(note.bytes, if (large) 112 else 64)
        }
    }
}

@Composable
fun HandwritingCanvas(bytes: ByteArray, cellDp: Int, maxCells: Int = Int.MAX_VALUE) {
    val drawing = remember(bytes) { runCatching { InkCodec.decode(bytes) }.getOrNull() }
    if (drawing == null) { Text("This handwriting could not be displayed."); return }
    val cells = drawing.cells.take(maxCells)
    BoxWithConstraints(Modifier.fillMaxWidth().padding(vertical = 8.dp)) {
        val columns = (maxWidth.value / cellDp).toInt().coerceAtLeast(1)
        val rows = (cells.size + columns - 1) / columns
        Canvas(Modifier.fillMaxWidth().height((rows * cellDp).dp).background(Color.White)) {
            val cell = size.width / columns
            val scale = minOf((cell - 8.dp.toPx()) / 160f, (cellDp.dp.toPx() - 8.dp.toPx()) / 144f).coerceAtLeast(.01f)
            cells.forEachIndexed { index, glyph ->
                val origin = Offset((index % columns) * cell + 4.dp.toPx(), (index / columns) * cellDp.dp.toPx() + 4.dp.toPx())
                glyph.strokes.forEach { stroke ->
                    stroke.forEachIndexed { i, point ->
                        val p = origin + Offset(point.x * scale, point.y * scale)
                        if (i == 0) drawCircle(Color.Black, 1.1.dp.toPx(), p)
                        else {
                            val previous = stroke[i - 1]
                            drawLine(Color.Black, origin + Offset(previous.x * scale, previous.y * scale), p, 2.2.dp.toPx(), StrokeCap.Round)
                        }
                    }
                }
            }
        }
    }
}
