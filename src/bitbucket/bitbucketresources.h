// Port of BitbucketResources.resx: the pages the loopback listener shows after browser sign-in (copied verbatim).
// The failure page is a .NET format string: {0} error, {1} description, {2} URI.
#pragma once

#include <QString>

namespace gcm::bitbucket::BitbucketResources {

inline const QString authenticationResponseSuccessHtml = QStringLiteral(R"gcm(<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="utf-8">
    <meta name="color-scheme" content="light dark">
    <title>Bitbucket Authentication</title>
    <link rel="stylesheet" href="http://aui-cdn.atlassian.com/aui-adg/5.9.19/css/aui.min.css" media="all">
    <script src="http://ajax.googleapis.com/ajax/libs/jquery/1.8.3/jquery.min.js"></script>
    <script src="http://aui-cdn.atlassian.com/aui-adg/5.9.19/js/aui.min.js"></script>
    <style>
        @media (prefers-color-scheme: dark) {
            html, body, body.aui-page-notification { background: #0D1117 !important; color: #C9D1D9 !important; }
            .aui-page-panel, .aui-page-panel-inner, .aui-page-panel-content { background: #161B22 !important; color: #C9D1D9 !important; border-color: #30363D !important; }
            h1, h2, h3, h4, h5, h6, p { color: #C9D1D9 !important; }
            a { color: #58A6FF !important; }
            #footer, #footer-logo { background: #0D1117 !important; color: #C9D1D9 !important; }
        }
    </style>
</head>
<body class="aui-page-notification aui-page-size-large">
	<div id="page" style="text-align: center;">
		<img src="https://wac-cdn.atlassian.com/dam/jcr:01f0ea80-2b31-4009-8eaf-da8ebcabdfe1/Bitbucket-blue.svg" height="23px" alt="Bitbucket" style="margin-top:50px">
        <div class="aui-page-panel">
            <div class="aui-page-panel-inner">
                <section class="aui-page-panel-content">
                    <h2>Authentication Successful</h2>
                    <p>Git Credential Manager has been successfully authenticated. You may now close this page.</p>
                </section>
            </div>
        </div>
        <footer id="footer" role="contentinfo">
            <section class="footer-body">
                <ul>
                    <li>&hearts;</li>
                </ul>
                <div id="footer-logo"><a href="http://www.atlassian.com/" target="_blank">Atlassian</a></div>
            </section>
        </footer>
    </div>
</body>
</html>)gcm");

inline const QString authenticationResponseFailureHtmlFormat = QStringLiteral(R"gcm(<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="utf-8">
    <meta name="color-scheme" content="light dark">
    <title>Bitbucket Authentication</title>
    <link rel="stylesheet" href="http://aui-cdn.atlassian.com/aui-adg/5.9.19/css/aui.min.css" media="all">
    <script src="http://ajax.googleapis.com/ajax/libs/jquery/1.8.3/jquery.min.js"></script>
	<script src="http://aui-cdn.atlassian.com/aui-adg/5.9.19/js/aui.min.js"></script>
	<style>dt {{ font-weight: bold; margin-top: 15px }} dd {{ margin-left: 0; }}
        @media (prefers-color-scheme: dark) {{
            html, body, body.aui-page-notification {{ background: #0D1117 !important; color: #C9D1D9 !important; }}
            .aui-page-panel, .aui-page-panel-inner, .aui-page-panel-content {{ background: #161B22 !important; color: #C9D1D9 !important; border-color: #30363D !important; }}
            h1, h2, h3, h4, h5, h6, p, dt, dd {{ color: #C9D1D9 !important; }}
            a {{ color: #58A6FF !important; }}
            #footer, #footer-logo {{ background: #0D1117 !important; color: #C9D1D9 !important; }}
        }}
    </style>
</head>
<body class="aui-page-notification aui-page-size-large">
	<div id="page" style="text-align: center;">
		<img src="https://wac-cdn.atlassian.com/dam/jcr:01f0ea80-2b31-4009-8eaf-da8ebcabdfe1/Bitbucket-blue.svg" height="23px" alt="Bitbucket" style="margin-top:50px">
        <div class="aui-page-panel">
            <div class="aui-page-panel-inner">
                <section class="aui-page-panel-content">
                    <div>
						<h2>Authentication Failed</h2>
						<p>Git Credential Manager failed to be authenticated.</p>
						<dl class="error">
							<dt>Error</dt>
							<dd>{0}</dt>
							<dt>Description</dt>
							<dd>{1}</dt>
							<dt>URL</dt>
							<dd>{2}</dt>
						</dl>
					</div>
				</section>
			</div>
        </div>
        <footer id="footer" role="contentinfo">
            <section class="footer-body">
                <ul>
                    <li>&hearts;</li>
                </ul>
                <div id="footer-logo"><a href="http://www.atlassian.com/" target="_blank">Atlassian</a></div>
            </section>
        </footer>
    </div>
</body>
</html>)gcm");

} // namespace gcm::bitbucket::BitbucketResources
