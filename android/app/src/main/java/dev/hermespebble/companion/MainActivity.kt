package dev.hermespebble.companion

import android.Manifest
import android.content.Intent
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import dev.hermespebble.companion.ui.HermesPebbleTheme
import androidx.compose.material3.Surface
import dev.hermespebble.companion.ui.AppViewModel
import dev.hermespebble.companion.ui.HermesApp

class MainActivity : ComponentActivity() {
    private val viewModel: AppViewModel by viewModels()

    private val notificationPermission = registerForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        val container = (application as HermesPt2Application).requireContainer()
        setContent {
            HermesPebbleTheme {
                Surface {
                    HermesApp(
                        viewModel = viewModel,
                        onRequestNotificationPermission = ::requestNotificationPermission,
                    )
                }
            }
        }
        container.dispatcher.setForegroundActive(true)
        openRequestedCommand(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        openRequestedCommand(intent)
    }

    override fun onStop() {
        (application as HermesPt2Application).requireContainer().dispatcher.setForegroundActive(false)
        super.onStop()
    }

    override fun onStart() {
        super.onStart()
        (application as HermesPt2Application).requireContainer().dispatcher.setForegroundActive(true)
    }

    private fun openRequestedCommand(intent: Intent?) {
        val id = intent?.getLongExtra(EXTRA_COMMAND_ID, -1L) ?: return
        if (id > 0) viewModel.openCommandIfPresent(id)
    }

    private fun requestNotificationPermission() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            notificationPermission.launch(Manifest.permission.POST_NOTIFICATIONS)
        }
    }

    companion object {
        const val EXTRA_COMMAND_ID = "command_id"
    }
}
