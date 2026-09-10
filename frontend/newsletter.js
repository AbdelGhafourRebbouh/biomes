// Submit data only after an explicit signup; never execute remote scripts or JSONP.
(() => {
    const form = document.querySelector('#newsletter-form');
    const email = document.querySelector('#newsletter-email');
    const button = form.querySelector('button[type="submit"]');
    const status = document.querySelector('#newsletter-status');
    const success = document.querySelector('#newsletter-success');
    const endpoint = 'https://assets.mailerlite.com/jsonp/1620793/forms/198235701885535513/subscribe';
    let pending = false;
    let subscribed = false;

    form.addEventListener('submit', async event => {
        event.preventDefault();
        if (pending || subscribed) return;
        email.value = email.value.trim();
        if (!form.reportValidity()) return;
        if (!navigator.onLine) {
            status.textContent = 'You’re offline. Connect to the internet, then try again.';
            return;
        }
        const submittedEmail = email.value;
        const controller = new AbortController();
        const timeout = setTimeout(() => controller.abort(), 15000);
        pending = true;
        button.disabled = true;
        email.readOnly = true;
        form.setAttribute('aria-busy', 'true');
        button.textContent = 'Sending…';
        status.textContent = 'Sending your signup to MailerLite…';
        try {
            const body = new URLSearchParams({
                'fields[email]': submittedEmail,
                'ml-submit': '1',
                'anticsrf': 'true',
                'ajax': '1'
            });
            const response = await fetch(endpoint, {
                method: 'POST', body, mode: 'cors', credentials: 'omit',
                referrerPolicy: 'no-referrer', signal: controller.signal
            });
            if (!response.ok) {
                status.textContent = response.status === 429
                    ? 'Too many attempts. Please wait a few minutes before trying again.'
                    : 'MailerLite could not confirm your signup. Please try again later.';
                return;
            }
            const result = await response.json();
            if (result.success === true) {
                subscribed = true;
                email.value = '';
                form.hidden = true;
                document.querySelector('#newsletter-optional').hidden = true;
                success.hidden = false;
                status.textContent = 'Check your inbox for a welcome email.';
                success.focus({preventScroll:true});
            } else {
                // Do not render remote HTML, redirects, or potentially sensitive server errors.
                status.textContent = 'MailerLite did not accept this signup. Check your email address and try again later.';
            }
        } catch {
            // An interrupted response does not prove the server rejected the signup.
            status.textContent = 'We couldn’t confirm your signup. Check your inbox before trying again, and make sure you’re online.';
        } finally {
            clearTimeout(timeout);
            pending = false;
            button.disabled = false;
            button.textContent = 'Subscribe';
            email.readOnly = false;
            form.removeAttribute('aria-busy');
        }
    });
})();
