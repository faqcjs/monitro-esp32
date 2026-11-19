-- Activity monitor schema for Supabase.
-- Run once in the Supabase dashboard: SQL Editor -> New query -> paste -> Run.
--
-- Security model:
--   * The web app signs in with Supabase Auth and only sees its own devices (RLS).
--   * The ESP32 does NOT sign in. It calls device_report() with its own secret token.
--     Only the SHA-256 hash of that token is stored.

create extension if not exists pgcrypto with schema extensions;

-- ---------------------------------------------------------------- tables

create table if not exists public.devices (
  id           uuid primary key default gen_random_uuid(),
  owner_id     uuid not null default auth.uid() references auth.users (id) on delete cascade,
  name         text not null default 'Monitor',
  token_hash   text not null unique,
  last_seen    timestamptz,
  rssi         int,
  uptime_min   int,
  alert_active boolean not null default false,
  created_at   timestamptz not null default now()
);

-- Defaults match the DEF_ values in the firmware.
create table if not exists public.settings (
  device_id      uuid primary key references public.devices (id) on delete cascade,
  inactivity_min int     not null default 30   check (inactivity_min between 1 and 10080),
  repeat_min     int     not null default 30   check (repeat_min between 1 and 1440),
  notify_each    boolean not null default true,
  motion_gap_min int     not null default 1    check (motion_gap_min between 0 and 1440),
  use_day_window boolean not null default false,
  day_start_h    int     not null default 8    check (day_start_h between 0 and 23),
  day_end_h      int     not null default 22   check (day_end_h between 0 and 23),
  updated_at     timestamptz not null default now()
);

create table if not exists public.events (
  id         bigint generated always as identity primary key,
  device_id  uuid not null references public.devices (id) on delete cascade,
  kind       text not null check (kind in ('boot', 'motion', 'alert', 'recovered')),
  created_at timestamptz not null default now()
);

create index if not exists events_device_time_idx on public.events (device_id, created_at desc);

-- ---------------------------------------------------------------- row level security

alter table public.devices  enable row level security;
alter table public.settings enable row level security;
alter table public.events   enable row level security;

create or replace function public.owns_device(p_device uuid)
returns boolean
language sql stable security definer set search_path = public
as $$
  select exists (select 1 from devices where id = p_device and owner_id = auth.uid());
$$;

drop policy if exists devices_select on public.devices;
create policy devices_select on public.devices
  for select to authenticated using (owner_id = auth.uid());

drop policy if exists devices_update on public.devices;
create policy devices_update on public.devices
  for update to authenticated using (owner_id = auth.uid()) with check (owner_id = auth.uid());

drop policy if exists devices_delete on public.devices;
create policy devices_delete on public.devices
  for delete to authenticated using (owner_id = auth.uid());

drop policy if exists settings_select on public.settings;
create policy settings_select on public.settings
  for select to authenticated using (public.owns_device(device_id));

drop policy if exists settings_update on public.settings;
create policy settings_update on public.settings
  for update to authenticated using (public.owns_device(device_id)) with check (public.owns_device(device_id));

drop policy if exists events_select on public.events;
create policy events_select on public.events
  for select to authenticated using (public.owns_device(device_id));

-- The web app may only rename a device; token and telemetry columns are written by RPCs.
revoke update on public.devices from authenticated;
grant update (name) on public.devices to authenticated;

-- ---------------------------------------------------------------- RPCs

-- Called from the web app. Creates a device and returns its secret token ONCE.
create or replace function public.register_device(p_name text default 'Monitor')
returns text
language plpgsql security definer set search_path = public, extensions
as $$
declare
  v_token text := encode(gen_random_bytes(24), 'hex');
  v_id    uuid;
begin
  if auth.uid() is null then
    raise exception 'not authenticated' using errcode = '28000';
  end if;
  insert into devices (owner_id, name, token_hash)
  values (auth.uid(), coalesce(nullif(trim(p_name), ''), 'Monitor'), encode(digest(v_token, 'sha256'), 'hex'))
  returning id into v_id;
  insert into settings (device_id) values (v_id);
  return v_token;
end;
$$;

-- Called from the ESP32: heartbeat + optional event. Returns the current settings.
create or replace function public.device_report(
  p_token        text,
  p_kind         text    default null,
  p_rssi         int     default null,
  p_uptime_min   int     default null,
  p_alert_active boolean default null
)
returns json
language plpgsql security definer set search_path = public, extensions
as $$
declare
  v_id uuid;
  s    settings;
begin
  select id into v_id from devices where token_hash = encode(digest(p_token, 'sha256'), 'hex');
  if v_id is null then
    raise exception 'invalid device token' using errcode = '28000';
  end if;

  update devices
     set last_seen    = now(),
         rssi         = coalesce(p_rssi, rssi),
         uptime_min   = coalesce(p_uptime_min, uptime_min),
         alert_active = coalesce(p_alert_active, alert_active)
   where id = v_id;

  if p_kind is not null then
    insert into events (device_id, kind) values (v_id, p_kind);
  end if;

  select * into s from settings where device_id = v_id;
  return json_build_object(
    'inactivity_min', s.inactivity_min,
    'repeat_min',     s.repeat_min,
    'notify_each',    s.notify_each,
    'motion_gap_min', s.motion_gap_min,
    'use_day_window', s.use_day_window,
    'day_start_h',    s.day_start_h,
    'day_end_h',      s.day_end_h
  );
end;
$$;

-- Called from the web app: motion events grouped by hour (RLS applies).
create or replace function public.motion_by_hour(p_device uuid, p_since timestamptz)
returns table (hour timestamptz, motions bigint)
language sql stable security invoker set search_path = public
as $$
  select date_trunc('hour', created_at) as hour, count(*) as motions
    from events
   where device_id = p_device and kind = 'motion' and created_at >= p_since
   group by 1
   order by 1;
$$;

revoke all on function public.register_device(text) from public, anon;
grant execute on function public.register_device(text) to authenticated;

revoke all on function public.device_report(text, text, int, int, boolean) from public;
grant execute on function public.device_report(text, text, int, int, boolean) to anon, authenticated;

revoke all on function public.motion_by_hour(uuid, timestamptz) from public, anon;
grant execute on function public.motion_by_hour(uuid, timestamptz) to authenticated;
