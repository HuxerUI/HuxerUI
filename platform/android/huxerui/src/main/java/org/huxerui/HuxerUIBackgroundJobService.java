package org.huxerui;

import android.app.job.JobParameters;
import android.app.job.JobService;
import android.util.Log;

/** Receives scheduled background tasks, including launches without an Activity. */
public final class HuxerUIBackgroundJobService extends JobService {
    @Override
    public boolean onStartJob(JobParameters parameters) {
        try {
            return HuxerUIApplication.initialize(this).systemTasks().beginJob(this, parameters);
        } catch (RuntimeException | LinkageError exception) {
            Log.e("HuxerUI", "HuxerUI could not initialize the application for its background job", exception);
            return false;
        }
    }

    @Override
    public boolean onStopJob(JobParameters parameters) {
        HuxerUIApplication application = HuxerUIApplication.current();
        if (application != null) application.systemTasks().stopJob(this, parameters);
        return false;
    }
}
