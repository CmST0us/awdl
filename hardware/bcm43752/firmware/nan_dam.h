#ifndef BCM43752_NAN_DAM_H
#define BCM43752_NAN_DAM_H

/* Native host-scheduled NAN storage. Protocol negotiation belongs to hostap.
 * Attach availability storage before initialize; release its peer entries
 * before deinitializing DAM, since native peer cleanup updates the DAM bitmap.
 */
int nan_dam_context_attach(void *nan);
int nan_dam_initialize(void *wlc, void *nan);
int nan_dam_deinitialize(void *wlc, void *nan);
int nan_dam_context_detach(void *nan);
int nan_dam_disable(void *wlc, void *nan);
int nan_dam_deactivate(void *dam, void *peer);
int nan_dam_activate(void *dam, void *peer);
int nan_dam_release_session(void *dam, void *peer);
int nan_dam_deactivate_complete(void *nan, void *peer);
int nan_dam_deactivate_ndl(void *ndl, void *peer, unsigned apply);
int nan_dam_transition_pending(void *dam);
void nan_dam_transition_reset(void *dam);
int nan_dam_check_peer_crb(void *dam, void *peer);
int nan_dam_parse_peer_schedule(void *dam, unsigned type, void *peer,
                                void *attributes, void *frame, unsigned force);
int nan_dam_update_social_channels(void *dam, unsigned dual_band);
int nan_dam_compatible_with_bt(void *dam, unsigned chanspec);
int nan_dam_valid_chanspec(void *dam, void *prohibited, unsigned chanspec,
                          unsigned checks);
int nan_dam_update_ndc_entry(void *dam, void *peer, int copy_peer_attribute);
int nan_dam_update_schedule(void *dam, unsigned radio, unsigned first,
                            unsigned last, unsigned channel, unsigned flags,
                            unsigned operation, unsigned commit);
int nan_dam_update_peer_ndl_slot(void *dam, void *peer, unsigned radio,
                               unsigned index, unsigned operation, unsigned apply);
int nan_dam_update_peer_ndl_slots(void *dam, void *peer, unsigned radio,
                                unsigned first, unsigned last,
                                unsigned operation, unsigned apply);
int nan_dam_update_for_excursion(void *dam, void *peer, unsigned radio,
                               unsigned operation, unsigned apply);
int nan_dam_watchdog_restore_slots(void *dam, void *peer);
int nan_dam_proposal_from_committed(void *dam, void *peer, void **maps,
                                    void **out, unsigned slots, unsigned type);
int nan_dam_proposal_from_potential(void *dam, void *peer, void **maps,
                                    void **out, unsigned slots);
int nan_dam_response_from_type(void *dam, void *peer, void **out,
                               void **maps, unsigned slots, unsigned type);
int nan_dam_proposal_ndc_slots(void *dam, void *peer, void **out, unsigned slots);
int nan_dam_response_ndc_slots(void *dam, void **out, unsigned slots);
int nan_dam_generate_proposal(void *dam, unsigned type, void *peer,
                              void **local, void **maps, void **out, unsigned slots);
int nan_dam_resize_map(void *nan, void *map, unsigned unit, unsigned period);
int nan_dam_resize_schedule(void *dam, unsigned unit, unsigned period);
int nan_dam_process_counter(void *dam, unsigned type, void *peer, void *result);
int nan_dam_get_proposal(void *dam, unsigned type, void *peer, void *result);
int nan_dam_process_request(void *dam, unsigned type, void *peer, void *result);
int nan_dam_process_response(void *dam, unsigned type, void *peer, void *result,
                              unsigned status);

#endif
