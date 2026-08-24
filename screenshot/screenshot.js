const puppeteer = require('puppeteer');

(async () => {

  const browser = await puppeteer.launch({
    headless: true,
    defaultViewport: {
      width: 2560,
      height: 1440,
      deviceScaleFactor: 2
    }
  });

  const page = await browser.newPage();

  await page.goto('http://localhost:5173', {
    waitUntil: 'networkidle2'
  });

  await page.screenshot({
    path: 'website.png',
    fullPage: true,
    type: 'png'
  });

  await browser.close();

})();