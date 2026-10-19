/* SPDX-License-Identifier: MIT */
static void check_scheduled_controller(void) {
    /* Desired public API is immediate; commits have no registry/event side effect. */
    struct led_pattern_state v;
    mock_desired=(struct led_sync_values){3, 71, 175, LED_SYNC_IDLE_OFF|LED_SYNC_INDICATOR};
    led_pattern_get_state(&v);
    assert(v.pattern==3 && v.speed==175 && v.idle_off && v.advertising_indicator);
    assert(led_pattern_get_brightness()==71);
    mock_desired.flags=0; led_pattern_get_state(&v);
    assert(!v.idle_off && !v.advertising_indicator);
    mock_brightness_changed=false;
    led_pattern_set_brightness(70);
    mock_brightness_changed=true; led_pattern_set_brightness(71);
    mock_state_changed=false; led_pattern_set_state(&v);
    apply_request(LED_PATTERN_FIELD_SPEED, 200);
    mock_state_changed=true; led_pattern_set_state(&v);
    apply_request(LED_PATTERN_FIELD_PATTERN, 3);
    assert(strstr(masking_indicator(),"scheduled")!=NULL);
    activity_state=ZMK_ACTIVITY_ACTIVE; apply_activity(); assert(mock_activity==1);
    activity_state=ZMK_ACTIVITY_IDLE; apply_activity(); assert(mock_activity==0);
    reported_activity=ZMK_ACTIVITY_ACTIVE;
    assert(behavior_led_pattern_init(NULL)==0 && mock_starts==1 && controller_ready);
    assert(mock_activity==1);
    const int combined_events = state_events;
    mock_state_changed=false; mock_brightness_changed=false;
    led_pattern_set_presentation(&v,44);
    assert(mock_desired.brightness==44 && state_events==combined_events);
    mock_brightness_changed=true; led_pattern_set_presentation(&v,45);
    assert(mock_desired.brightness==45 && state_events==combined_events+1);
    mock_brightness_changed=false;
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    v.advertising_indicator=true; led_pattern_set_state(&v);
    selected_transport=ZMK_TRANSPORT_NONE; profile_connected=false;
    refresh_connection_state(); assert(mock_endpoint==1);
    profile_connected=true; refresh_connection_state(); assert(mock_endpoint==0);
#endif
    mock_view=(struct led_scheduled_view){.values={3,0,100,LED_SYNC_ACTIVE}, .stage=LED_SYNC_WAITING,
        .status_started_at=100, .applied_sequence=7};
    now_ms=100; refresh_pattern_output(); assert(run_pattern() && led_level==100 && pattern_work.delay_ms==500);
    now_ms=600; run_pattern(); assert(led_level==0 && pattern_work.delay_ms==500);
    now_ms=599; run_pattern(); assert(led_level==100 && pattern_work.delay_ms==1);
    mock_view.stage=LED_SYNC_LINKED; run_pattern(); assert(led_level==100 && !pattern_work.scheduled);
    mock_view.values.flags=LED_SYNC_IDLE_OFF; run_pattern(); assert(led_level==0 && !pattern_work.scheduled);
    mock_view.normal=true;
    run_pattern(); assert(led_level==0 && mock_completed_sequence==7 && mock_completed_ok);
    mock_view.values.flags=LED_SYNC_ACTIVE; run_pattern(); assert(led_level==0);
    mock_view.values=(struct led_sync_values){3,50,100,LED_SYNC_ACTIVE};
    mock_view.elapsed_ms=250; run_pattern(); assert(led_level==50 && pattern_work.delay_ms==250);
    mock_view.elapsed_ms=500; run_pattern(); assert(led_level==2 && pattern_work.delay_ms==500);
    mock_view.values.pattern=LED_PATTERN_STEADY;
    run_pattern(); assert(led_level==50 && !pattern_work.scheduled);
    mock_view.values.flags|=LED_SYNC_ADVERTISING;
    mock_view.elapsed_ms=0; run_pattern(); assert(led_level==35 && pattern_work.delay_ms==300);
    mock_view.elapsed_ms=300; run_pattern(); assert(led_level==0 && pattern_work.delay_ms==1200);
    mock_view.values.flags=LED_SYNC_ACTIVE;
    mock_view.values.pattern=LED_PATTERN_SAWTOOTH; mock_view.values.brightness=100;
    mock_view.elapsed_ms=1799; run_pattern(); assert(pattern_work.delay_ms==1);
    backlight_device.ready=false; run_pattern(); assert(!mock_completed_ok);
    backlight_device.ready=true; led_result=-EIO; run_pattern(); assert(!mock_completed_ok);
    led_result=0; run_pattern(); assert(mock_completed_ok);
    mock_view.values.pattern=LED_PATTERN_SPARKLE; mock_view.values.speed=400;
    mock_view.elapsed_ms=56; run_pattern(); assert(pattern_work.delay_ms==1);
    int events=state_events;
    /* Every curve's phase stays exact beyond49days and at the wire's int64 limit.
     * Periods independently guard the production common-period reduction. */
    static const uint32_t periods[18]={1,2000,1200,1000,300,1500,3400,1080,1800,2200,16000,200,1600,1200,1050,2500,1320,1200};
    static const uint64_t times[]={UINT64_C(5000000000),INT64_MAX};
    static const uint16_t speeds[]={10,33,100,175,400};
    for(unsigned p=0;p<18;p++) for(unsigned t=0;t<ARRAY_SIZE(times);t++) for(unsigned sp=0;sp<ARRAY_SIZE(speeds);sp++) {
        mock_view.values.pattern=(uint8_t)p; mock_view.values.speed=speeds[sp]; mock_view.elapsed_ms=times[t];
        __uint128_t scaled=(__uint128_t)times[t]*speeds[sp]/100;
        int64_t phase=(int64_t)(scaled%periods[p]);
        struct pattern_sample expected=pattern_sample_at((uint8_t)p,phase);
        run_pattern(); assert(led_level==expected.brightness);
        uint32_t delay=expected.hold_ms ? MAX(1U,(uint32_t)((uint64_t)expected.hold_ms*100/speeds[sp])) : 0;
        assert(pattern_work.scheduled==(delay!=0));
        if(delay) assert(pattern_work.delay_ms==delay);
    }
    assert(state_events==events); /* Presentation never publishes stale desired values. */
    pattern_work.scheduled=true; led_level=77;
    assert(led_pattern_pm_action(NULL,PM_DEVICE_ACTION_SUSPEND)==0 && led_level==0 && !pattern_work.scheduled && mock_pm==1);
    pattern_work_handler(NULL); assert(!pattern_work.scheduled);
    assert(led_pattern_pm_action(NULL,PM_DEVICE_ACTION_SUSPEND)==0 && mock_pm==1);
    assert(led_pattern_pm_action(NULL,PM_DEVICE_ACTION_RESUME)==0 && mock_pm==0 && pattern_work.scheduled);
    assert(led_pattern_pm_action(NULL,PM_DEVICE_ACTION_RESUME)==0 && mock_pm==0);
    assert(led_pattern_pm_action(NULL,PM_DEVICE_ACTION_TURN_OFF)==-ENOTSUP);
    /* Public split callers reject invalid selectors before adapter forwarding.
     * A forwarding call would make this controlled adapter request a notification. */
    mock_state_changed = true;
    const int invalid_events = state_events;
    static const enum led_pattern_field invalid_fields[] = {
        (enum led_pattern_field)-1, (enum led_pattern_field)3, (enum led_pattern_field)INT32_MAX,
    };
    for (unsigned i = 0; i < ARRAY_SIZE(invalid_fields); ++i)
        led_pattern_request(invalid_fields[i], 123);
    assert(state_events == invalid_events);
    puts("scheduled controller desired/render/phase/PM: PASS");
}
