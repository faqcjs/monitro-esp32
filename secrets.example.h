// Copiar este archivo como "secrets.h" (en la misma carpeta) y completar los valores.
// secrets.h no debe compartirse ni subirse a un repositorio.
#pragma once

const char* const WIFI_SSID = "mi-red";
const char* const WIFI_PASS = "mi-clave";

// Tema de ntfy para las notificaciones push (largo e inventado: funciona como contraseña).
const char* const NTFY_TOPIC = "monitor-xxxxxxxxxxxx";

// Supabase: Project Settings -> API
const char* const SUPABASE_URL      = "https://xxxxxxxxxxxx.supabase.co";
const char* const SUPABASE_ANON_KEY = "sb_publishable_...";

// Token del aparato: lo muestra la app web UNA sola vez al registrar el aparato.
const char* const DEVICE_TOKEN = "pegar-aqui-el-token";
